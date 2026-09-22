import assert from "node:assert/strict";
import {spawn} from "node:child_process";
import {constants as fsConstants} from "node:fs";
import {access, mkdtemp, readFile, rm, writeFile} from "node:fs/promises";
import {createServer} from "node:http";
import {tmpdir} from "node:os";
import {dirname, extname, join, resolve, sep} from "node:path";
import {fileURLToPath} from "node:url";
import {timingLimit} from "./timing-policy.mjs";

const toolDirectory = dirname(fileURLToPath(import.meta.url));
const artifactDirectory = resolve(toolDirectory, "../app-dist");
const wait = milliseconds => new Promise(complete => setTimeout(complete, milliseconds));

async function chromeExecutable() {
    const candidates = [process.env.CHROME_BIN, "/opt/google/chrome/chrome", "/usr/bin/google-chrome",
        "/usr/bin/google-chrome-stable", "/usr/bin/chromium", "/usr/bin/chromium-browser"].filter(Boolean);
    for (const candidate of candidates) {
        try { await access(candidate, fsConstants.X_OK); return candidate; }
        catch { /* Try the next canonical installation path. */ }
    }
    throw new Error("Real-browser qualification requires Chrome or Chromium; set CHROME_BIN to its executable");
}

function contentType(path) {
    return ({".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8",
        ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml"})[extname(path)] ?? "application/octet-stream";
}

async function staticServer() {
    const server = createServer(async (request, response) => {
        try {
            const pathname = decodeURIComponent(new URL(request.url ?? "/", "http://127.0.0.1").pathname);
            const path = resolve(artifactDirectory, pathname === "/" ? "index.html" : `.${pathname}`);
            if (path !== artifactDirectory && !path.startsWith(`${artifactDirectory}${sep}`)) throw new Error("invalid path");
            const body = await readFile(path);
            response.writeHead(200, {"content-type": contentType(path), "cache-control": "no-store"});
            response.end(body);
        } catch {
            response.writeHead(404, {"content-type": "text/plain; charset=utf-8"});
            response.end("Not found");
        }
    });
    server.on("upgrade", (_request, socket) => socket.destroy());
    await new Promise((complete, reject) => {
        server.once("error", reject);
        server.listen(0, "127.0.0.1", complete);
    });
    const address = server.address();
    assert(address && typeof address === "object");
    return {server, url: `http://127.0.0.1:${address.port}/`};
}

async function availableLoopbackPort() {
    const probe = createServer();
    await new Promise((complete, reject) => {
        probe.once("error", reject);
        probe.listen(0, "127.0.0.1", complete);
    });
    const address = probe.address();
    assert(address && typeof address === "object");
    await new Promise((complete, reject) => probe.close(error => error ? reject(error) : complete()));
    return address.port;
}

class DevTools {
    #id = 0;
    #pending = new Map();
    #socket;

    constructor(socket) {
        this.#socket = socket;
        socket.addEventListener("message", event => {
            const message = JSON.parse(String(event.data));
            const pending = this.#pending.get(message.id);
            if (!pending) return;
            this.#pending.delete(message.id);
            if (message.error) pending.reject(new Error(message.error.message));
            else pending.resolve(message.result);
        });
    }

    static async connect(url) {
        const socket = new WebSocket(url);
        await new Promise((complete, reject) => {
            socket.addEventListener("open", complete, {once: true});
            socket.addEventListener("error", reject, {once: true});
        });
        return new DevTools(socket);
    }

    call(method, params = {}) {
        const id = ++this.#id;
        return new Promise((resolveCall, reject) => {
            this.#pending.set(id, {resolve: resolveCall, reject});
            this.#socket.send(JSON.stringify({id, method, params}));
        });
    }

    async evaluate(expression) {
        const result = await this.call("Runtime.evaluate", {expression, awaitPromise: true, returnByValue: true});
        if (result.exceptionDetails) throw new Error(result.exceptionDetails.exception?.description
            ?? result.exceptionDetails.text ?? "Browser evaluation failed");
        return result.result.value;
    }

    close() { this.#socket.close(); }
}

async function pageTarget(port) {
    const deadline = Date.now() + 15_000;
    while (Date.now() < deadline) {
        try {
            const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
            const page = targets.find(target => target.type === "page");
            if (page) return page.webSocketDebuggerUrl;
        } catch { /* Chrome may still be starting. */ }
        await wait(25);
    }
    throw new Error("Chrome did not expose the CodexUI page target");
}

async function setWidth(devTools, width) {
    await devTools.call("Emulation.setDeviceMetricsOverride", {width, height: 900, deviceScaleFactor: 1, mobile: false});
    await waitUntil(devTools, `innerWidth===${width}`, `${width}px viewport`);
}

async function waitUntil(devTools, expression, description) {
    for (let attempt = 0; attempt < 100; ++attempt) {
        if (await devTools.evaluate(`Boolean(${expression})`)) return;
        await wait(25);
    }
    throw new Error(`Timed out waiting for ${description}`);
}

async function performanceSnapshot(devTools) {
    const {metrics} = await devTools.call("Performance.getMetrics");
    return Object.fromEntries(metrics.map(metric => [metric.name, metric.value]));
}

function performanceDelta(before, after) {
    const duration = name => Number(((after[name] - before[name]) * 1000).toFixed(2));
    return {
        taskMilliseconds: duration("TaskDuration"),
        scriptMilliseconds: duration("ScriptDuration"),
        layoutMilliseconds: duration("LayoutDuration"),
        styleMilliseconds: duration("RecalcStyleDuration"),
        layouts: after.LayoutCount - before.LayoutCount,
        styleRecalculations: after.RecalcStyleCount - before.RecalcStyleCount,
    };
}

const applicationPerformanceLimits = Object.freeze({
    hydrateWallMilliseconds: 141,
    hydrateTaskMilliseconds: 154,
    idleTaskMilliseconds: 1,
    semanticNoOpTaskMilliseconds: 25,
    streamIngestMilliseconds: 20,
    streamSettledMilliseconds: 47,
    streamTaskMilliseconds: 48,
});
const performanceFailures = [];
function checkPerformance(condition, message) {
    if (!condition) performanceFailures.push(message);
}
function checkTiming(condition, message) {
    checkPerformance(timingLimit(condition, message), message);
}

const applicationProfileSetup = `(async()=>{
    const delay=milliseconds=>new Promise(complete=>setTimeout(complete,milliseconds));
    const waitFor=async(predicate,description)=>{for(let attempt=0;attempt<400;++attempt){
        if(predicate())return;await delay(5);}throw new Error("Timed out waiting for "+description);};
    class ProfileWebSocket{
        constructor(){this.protocol="codex";this.readyState=0;this.bufferedAmount=0;this.binaryType="arraybuffer";
            this.onopen=null;this.onmessage=null;this.onerror=null;this.onclose=null;this.sent=[];
            this.pendingCatalogs=[];this.turnPages=0;this.itemPages=0;this.deliveredItems=0;
            globalThis.codexuiProfileSocket=this;queueMicrotask(()=>{this.readyState=1;this.onopen?.();});}
        send(data){const message=JSON.parse(data);this.sent.push(message);const payload=message.kind==="appserver"?message.payload:null;
            if(!payload?.method||payload.method==="thread/resume")return;
            if(payload.method==="thread/read")throw new Error("Profile hydration must use pagination");
            if(payload.method==="thread/turns/list"||payload.method==="thread/items/list"){
                this.historyPage(payload);
                return;}
            const result=payload.method==="thread/list"?{data:[{id:"profile",status:{type:"idle"}}],nextCursor:null}
                :payload.method==="model/list"?{data:[
                    {id:"gpt-a",model:"gpt-a",displayName:"GPT A",description:"Primary model",isDefault:true,
                        supportedReasoningEfforts:[{reasoningEffort:"low"},{reasoningEffort:"medium"}],
                        defaultReasoningEffort:"medium",serviceTiers:[{id:"fast",name:"Fast",description:"Fast tier"}],
                        defaultServiceTier:"fast",supportsPersonality:true},
                    {id:"gpt-b",model:"gpt-b",displayName:"GPT B",description:"No style model",
                        supportedReasoningEfforts:[{reasoningEffort:"medium"}],supportsPersonality:false}]}
                :payload.method==="permissionProfile/list"?{data:[{id:":workspace",description:"Workspace access"}]}:{data:[]};
            if(payload.method==="model/list"||payload.method==="permissionProfile/list"){
                this.pendingCatalogs.push({id:payload.id,result});return;}
            queueMicrotask(()=>this.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:payload.id,result}}));}
        historyPage(payload){
            const params=payload.params;const turns=payload.method==="thread/turns/list";
            if(params.threadId!=="profile"||params.limit!==80||params.sortDirection!=="desc"
                ||(turns&&params.itemsView!=="summary"))throw new Error("Unexpected history page parameters");
            const source=turns?this.turns:this.turns.find(turn=>turn.id===params.turnId)?.items;
            if(!source)throw new Error("Unknown history turn");
            const offset=Number(params.cursor??0);const page=source.toReversed().slice(offset,offset+params.limit);
            const data=turns?page.map(({items,...summary})=>({...summary,items:[]}))
                :page.map(item=>({turnId:params.turnId,item}));
            const nextCursor=offset+page.length<source.length?String(offset+page.length):null;
            if(turns)++this.turnPages;else{++this.itemPages;this.deliveredItems+=data.length;}
            queueMicrotask(()=>this.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:payload.id,
                result:{data,nextCursor}}}));}
        releaseCatalogs(){for(const pending of this.pendingCatalogs)
            this.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:pending.id,result:pending.result}});
            this.pendingCatalogs=[];}
        receive(message){this.onmessage?.({data:JSON.stringify(message)});}
        close(_code,reason=""){this.readyState=3;this.onclose?.({reason});}
    }
    ProfileWebSocket.CONNECTING=0;ProfileWebSocket.OPEN=1;ProfileWebSocket.CLOSING=2;ProfileWebSocket.CLOSED=3;
    globalThis.WebSocket=ProfileWebSocket;
    document.querySelector(".connection-control button").click();
    await waitFor(()=>globalThis.codexuiProfileSocket?.readyState===1,"profile WebSocket");
    const socket=globalThis.codexuiProfileSocket;
    socket.receive({kind:"bridge.connection",event:"opened",connectionId:"profile",role:"controller"});
    socket.receive({kind:"bridge.provider",state:"ready",providerGeneration:1});
    await waitFor(()=>document.querySelector(".thread-row"),"profile thread row");
    const reasoning=document.querySelector('[aria-label="Show reasoning cards"]');
    reasoning?.click();
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    document.querySelector(".thread-row").click();
    await waitFor(()=>socket.sent.some(message=>message.kind==="appserver"&&message.payload.method==="thread/resume"),
        "profile metadata resume");
    const resume=socket.sent.findLast(message=>message.kind==="appserver"&&message.payload.method==="thread/resume");
    await waitFor(()=>socket.pendingCatalogs.length===2,"deferred settings catalogs");
    const settingsToggle=document.querySelector(".settings-toggle");settingsToggle.click();
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const grid=document.querySelector(".settings-grid");
    const selectFor=caption=>[...grid.querySelectorAll("label")].find(
        label=>label.querySelector("span")?.textContent===caption)?.querySelector("select");
    const modelSelect=selectFor("Model");const profileSelect=selectFor("Permission profile");
    socket.releaseCatalogs();
    await waitFor(()=>modelSelect?.querySelector('option[value="gpt-a"]')&&
        profileSelect?.querySelector('option[value=":workspace"]'),"visible settings catalog refresh");
    const catalogUpdated=modelSelect===selectFor("Model")&&profileSelect===selectFor("Permission profile");
    settingsToggle.click();
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    globalThis.codexuiApplicationProfile={socket,resume};
    return {resumeRequests:socket.sent.filter(message=>message.kind==="appserver"&&message.payload.method==="thread/resume").length,
        resumeParams:resume.payload.params,catalogUpdated};
})()`;

const exactInteractionLifetimeCheck = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;const socket=profile.socket;
    const editor=document.querySelector('.composer textarea');
    const setEditorValue=Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value').set;
    setEditorValue.call(editor,'retain authored text');editor.dispatchEvent(new Event('input',{bubbles:true}));
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    const turnStartsBefore=socket.sent.filter(message=>message.kind==='appserver'&&message.payload.method==='turn/start').length;
    socket.receive({kind:'bridge.controller',controllerConnectionId:'another-client'});
    editor.form.requestSubmit();
    const composerImmediate=editor.value;
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const composerSettled=editor.value;const turnStartsAfter=socket.sent.filter(
        message=>message.kind==='appserver'&&message.payload.method==='turn/start').length;
    socket.receive({kind:'bridge.controller',controllerConnectionId:'profile'});
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    setEditorValue.call(editor,'');editor.dispatchEvent(new Event('input',{bubbles:true}));

    const receiveThread=(id,preview,parentThreadId)=>socket.receive({kind:'appserver',payload:{jsonrpc:'2.0',
        method:'thread/started',params:{thread:{id,preview,status:{type:'idle'},...(parentThreadId?{parentThreadId}:{})}}}});
    receiveThread('reparent-parent','Reparent parent');receiveThread('reparent-child','Reparent child','reparent-parent');
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const treeItem=title=>[...document.querySelectorAll('.thread-row strong')]
        .find(label=>label.textContent===title)?.closest('[role="treeitem"]');
    treeItem('Reparent parent').querySelector('.tree-toggle').click();
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    const childBefore=treeItem('Reparent child');const triggerBefore=childBefore.querySelector('.thread-menu-trigger');
    triggerBefore.focus();const levelBefore=childBefore.getAttribute('aria-level');
    socket.receive({kind:'appserver',payload:{jsonrpc:'2.0',method:'thread/deleted',params:{threadId:'reparent-parent'}}});
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const childAfter=treeItem('Reparent child');const triggerAfter=childAfter.querySelector('.thread-menu-trigger');
    const rowRetained=childAfter===childBefore&&triggerAfter===triggerBefore;
    const focusRetained=document.activeElement===triggerBefore;const levelAfter=childAfter.getAttribute('aria-level');
    triggerAfter.click();await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const menu=document.querySelector('.thread-context-menu');const menuFocused=menu?.contains(document.activeElement)??false;
    document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true,cancelable:true}));
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    const menuClosed=!document.querySelector('.thread-context-menu');
    const focusReturned=document.activeElement===triggerAfter&&triggerAfter.isConnected;
    socket.receive({kind:'appserver',payload:{jsonrpc:'2.0',method:'thread/deleted',params:{threadId:'reparent-child'}}});
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    return {composerImmediate,composerSettled,turnStartsBefore,turnStartsAfter,rowRetained,focusRetained,
        levelBefore,levelAfter,menuFocused,menuClosed,focusReturned};
})()`;

const applicationProfileHydrate = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;
    // One bounded turn page retains the original 10,000-item / 80-card initial-render workload.
    const turns=Array.from({length:80},(_,turn)=>({id:"turn-"+turn,status:turn===79?"inProgress":"completed",
        items:Array.from({length:125},(_,item)=>({id:"item-"+turn+"-"+item,
            type:(item+2)%3===0?"agentMessage":(item+2)%3===1?"reasoning":"commandExecution",
            text:(item+2)%3===0?"Answer "+item:undefined,summary:(item+2)%3===1?["Thinking"]:undefined,
            command:(item+2)%3===2?"true":undefined,status:"completed"}))}));
    profile.socket.turns=turns;
    const started=performance.now();
    profile.socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:profile.resume.payload.id,result:{thread:{
        id:"profile",model:"gpt-a",approvalPolicy:"future-policy",status:{type:"active"}}}}});
    for(let attempt=0;profile.socket.deliveredItems!==10000;++attempt){
        if(attempt===400)throw new Error("Timed out waiting for paginated items");
        await new Promise(complete=>setTimeout(complete,5));}
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    return {wallMilliseconds:performance.now()-started,authoritativeItems:profile.socket.deliveredItems,
        turnPages:profile.socket.turnPages,itemPages:profile.socket.itemPages,
        visibleCards:document.querySelectorAll(".conversation-card").length,
        hasHistoryBoundary:Boolean(document.querySelector(".load-more"))};
})()`;

const applicationProfilePrepare = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;
    document.querySelector(".settings-toggle").click();
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const grid=document.querySelector(".settings-grid");if(!grid)throw new Error("Settings grid did not open");
    const controls=[...grid.querySelectorAll("select,input")];const options=[...grid.querySelectorAll("option")];
    controls[0]?.focus();
    const cards=[...document.querySelectorAll(".conversation-card")];const targetCard=cards.at(-1);
    const targetText=targetCard?.querySelector(".safe-markdown p")?.firstChild;
    const scroll=document.querySelector(".conversation-scroll");
    if(!targetCard||!targetText)throw new Error("Stream target did not render");
    const mutations={settingsStructuralMutations:0,conversationStructuralMutations:0,targetTextMutations:0};
    const settingsObserver=new MutationObserver(records=>{for(const record of records)
        if(record.type==="childList")mutations.settingsStructuralMutations+=record.addedNodes.length+record.removedNodes.length;});
    settingsObserver.observe(grid,{childList:true,subtree:true});
    const conversationObserver=new MutationObserver(records=>{for(const record of records){
        if(record.type==="childList")mutations.conversationStructuralMutations+=record.addedNodes.length+record.removedNodes.length;
        else if(record.target===targetText)++mutations.targetTextMutations;}});
    conversationObserver.observe(document.querySelector(".conversation-scroll"),{childList:true,characterData:true,subtree:true});
    profile.grid=grid;profile.controls=controls;profile.options=options;profile.cards=cards;
    profile.targetCard=targetCard;profile.targetText=targetText;profile.settingsObserver=settingsObserver;
    profile.conversationObserver=conversationObserver;profile.mutations=mutations;
    profile.activityText=document.querySelector(".conversation-activity")?.textContent||"";
    profile.scroll=scroll;profile.focused=controls[0];profile.scrollTop=scroll.scrollTop;
    profile.followingGap=scroll.scrollHeight-scroll.scrollTop-scroll.clientHeight;
    return {settingsControls:controls.length};
})()`;

const applicationProfileNoOp = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;
    profile.socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",method:"thread/settings/updated",params:{
        threadId:"profile",threadSettings:{model:"gpt-a"}}}});
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const grid=profile.grid;const controls=profile.controls;const options=profile.options;
    const nextControls=[...grid.querySelectorAll("select,input")];const nextOptions=[...grid.querySelectorAll("option")];
    return {gridStable:grid===document.querySelector(".settings-grid"),
        controlsStable:controls.length===nextControls.length&&controls.every((node,index)=>node===nextControls[index]),
        optionsStable:options.length===nextOptions.length&&options.every((node,index)=>node===nextOptions[index]),
        focusStable:document.activeElement===profile.focused,scrollStable:profile.scroll.scrollTop===profile.scrollTop,
        activityBefore:profile.activityText,
        activityAfter:document.querySelector(".conversation-activity")?.textContent||"",
        ...profile.mutations};
})()`;

const applicationProfileStream = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;profile.mutations.settingsStructuralMutations=0;
    profile.mutations.conversationStructuralMutations=0;profile.mutations.targetTextMutations=0;
    const beforeText=profile.targetText.data;const started=performance.now();
    for(let index=0;index<2000;++index)profile.socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",
        method:"item/agentMessage/delta",params:{threadId:"profile",turnId:"turn-79",itemId:"item-79-124",delta:"x"}}});
    const ingestMilliseconds=performance.now()-started;
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const cards=[...document.querySelectorAll(".conversation-card")];const controls=[...profile.grid.querySelectorAll("select,input")];
    const options=[...profile.grid.querySelectorAll("option")];profile.settingsObserver.disconnect();profile.conversationObserver.disconnect();
    return {ingestMilliseconds,settledMilliseconds:performance.now()-started,streamedDeltas:2000,
        textGrowth:profile.targetText.data.length-beforeText.length,targetCardStable:profile.targetCard===cards.at(-1),
        targetTextStable:profile.targetText===profile.targetCard.querySelector(".safe-markdown p")?.firstChild,
        cardsStable:profile.cards.length===cards.length&&profile.cards.every((node,index)=>node===cards[index]),
        focusStable:document.activeElement===profile.focused,followingGapBefore:profile.followingGap,
        followingGapAfter:profile.scroll.scrollHeight-profile.scroll.scrollTop-profile.scroll.clientHeight,
        controlsStable:profile.controls.length===controls.length&&profile.controls.every((node,index)=>node===controls[index]),
        optionsStable:profile.options.length===options.length&&profile.options.every((node,index)=>node===options[index]),
        ...profile.mutations};
})()`;

const applicationProfileDetachedStream = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;const scroll=profile.scroll;
    const anchor=profile.cards.at(-30);const source=profile.cards.at(-58);
    const contentTop=anchor.getBoundingClientRect().top-scroll.getBoundingClientRect().top+scroll.scrollTop;
    scroll.scrollTop=contentTop-32;scroll.dispatchEvent(new Event("scroll"));
    await new Promise(complete=>requestAnimationFrame(()=>complete()));
    const beforeTop=anchor.getBoundingClientRect().top-scroll.getBoundingClientRect().top;
    const beforeGap=scroll.scrollHeight-scroll.scrollTop-scroll.clientHeight;
    const sourceText=source.querySelector(".safe-markdown p")?.firstChild;const beforeLength=sourceText?.data.length||0;
    profile.socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",method:"item/agentMessage/delta",params:{
        threadId:"profile",turnId:"turn-79",itemId:"item-79-67",delta:" detached".repeat(300)}}});
    await new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    return {anchorStable:anchor===profile.cards.at(-30),anchorTopDelta:Math.abs(
        anchor.getBoundingClientRect().top-scroll.getBoundingClientRect().top-beforeTop),
        detachedBefore:beforeGap,detachedAfter:scroll.scrollHeight-scroll.scrollTop-scroll.clientHeight,
        sourceGrowth:(sourceText?.data.length||0)-beforeLength};
})()`;

const applicationSettingsDomContract = `(async()=>{
    const profile=globalThis.codexuiApplicationProfile;const grid=profile.grid;
    const frame=()=>new Promise(complete=>requestAnimationFrame(()=>requestAnimationFrame(()=>complete())));
    const waitFor=async(predicate,description)=>{for(let attempt=0;attempt<200;++attempt){
        if(predicate())return;await new Promise(complete=>setTimeout(complete,5));}
        throw new Error("Timed out waiting for "+description);};
    const labelFor=caption=>[...grid.querySelectorAll("label")].find(
        label=>label.querySelector(":scope > span")?.textContent===caption);
    const controlFor=caption=>labelFor(caption)?.querySelector("select,input");
    const setValue=async(caption,value)=>{const control=controlFor(caption);
        if(!control)throw new Error("Missing settings control: "+caption);
        const prototype=control instanceof HTMLSelectElement?HTMLSelectElement.prototype:HTMLInputElement.prototype;
        Object.getOwnPropertyDescriptor(prototype,"value").set.call(control,value);
        control.dispatchEvent(new Event(control instanceof HTMLSelectElement?"change":"input",{bubbles:true}));
        await frame();return controlFor(caption);};
    const captions=["Model","Reasoning","Access","Network","Workspace","Approval","Style",
        "Approval reviewer","Permission profile","Service tier","Reasoning summary","Collaboration mode"];
    const labelsAssociated=captions.every(caption=>{const label=labelFor(caption);const control=controlFor(caption);
        return Boolean(label&&control&&label.control===control);});
    const modelDescription=controlFor("Model").querySelector('option[value="gpt-a"]')?.title;
    const profileDescription=controlFor("Permission profile").querySelector('option[value=":workspace"]')?.title;
    const approval=controlFor("Approval");
    const unknownApproval={value:approval.value,label:approval.selectedOptions[0]?.textContent};

    await setValue("Model","gpt-b");
    const unsupportedStyle={disabled:controlFor("Style").disabled,title:labelFor("Style").title};
    await setValue("Model","gpt-a");
    await setValue("Style","friendly");
    await setValue("Reasoning","low");
    await setValue("Approval","never");
    await setValue("Approval reviewer","auto_review");
    await setValue("Service tier","fast");
    await setValue("Reasoning summary","concise");
    await setValue("Collaboration mode","plan");
    await setValue("Workspace","/authored/browser");
    await setValue("Access","danger-full-access");
    const fullAccessNetwork={value:controlFor("Network").value,disabled:controlFor("Network").disabled,
        title:labelFor("Network").title};
    await setValue("Permission profile",":workspace");
    const profileSelected={value:controlFor("Permission profile").value,access:controlFor("Access").value,
        network:controlFor("Network").value};
    await setValue("Access","workspace-write");
    await setValue("Network","enabled");
    const changedText=document.querySelector(".settings-toggle")?.textContent||"";

    profile.socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",method:"turn/completed",params:{
        threadId:"profile",turn:{id:"turn-79",status:"completed"}}}});
    await frame();
    const editor=document.querySelector(".composer textarea");
    Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,"value").set.call(editor,"settings DOM contract");
    editor.dispatchEvent(new Event("input",{bubbles:true}));await frame();
    const previous=profile.socket.sent.filter(message=>message.kind==="appserver"&&message.payload.method==="turn/start").length;
    editor.form.requestSubmit();
    await waitFor(()=>profile.socket.sent.filter(message=>message.kind==="appserver"&&
        message.payload.method==="turn/start").length===previous+1,"settings turn/start");
    const request=profile.socket.sent.filter(message=>message.kind==="appserver"&&message.payload.method==="turn/start").at(-1);
    return {labelsAssociated,modelDescription,profileDescription,unknownApproval,unsupportedStyle,
        fullAccessNetwork,profileSelected,changedText,params:request.payload.params};
})()`;

const fullTopBarMeasurement = `(()=>{
    const actions=document.querySelector(".top-actions");
    if(!actions.querySelector(".qualification-control")){
        const button=document.createElement("button");
        button.className="subtle-button qualification-control";
        button.textContent="Release control";
        actions.prepend(button);
    }
    const top=document.querySelector(".top-bar").getBoundingClientRect();
    const actionBounds=actions.getBoundingClientRect();
    return {width:innerWidth,overflow:document.documentElement.scrollWidth-document.documentElement.clientWidth,
        top:{left:top.left,right:top.right,bottom:top.bottom,height:top.height},
        actions:{left:actionBounds.left,right:actionBounds.right,bottom:actionBounds.bottom},
        drawers:[...document.querySelectorAll(".responsive-pane-button")].map(button=>button.textContent.trim())};
})()`;

function assertTopBar(measurement, width) {
    assert.equal(measurement.width, width);
    assert.equal(measurement.overflow, 0, `${width}px layout must not overflow the document`);
    assert(measurement.top.left >= 0 && measurement.top.right <= width);
    assert(measurement.actions.left >= 0 && measurement.actions.right <= width);
    assert(measurement.actions.bottom <= measurement.top.bottom + 0.5);
}

await access(join(artifactDirectory, "index.html"), fsConstants.R_OK);
const executable = await chromeExecutable();
const {server, url} = await staticServer();
const profile = await mkdtemp(join(tmpdir(), "codexui-browser-qualification."));
let chrome;
let chromeErrors = "";
let devTools;

try {
    const port = await availableLoopbackPort();
    const constrainedRunnerArguments = process.env.CI
        ? ["--no-sandbox", "--disable-dev-shm-usage"]
        : [];
    chrome = spawn(executable, ["--headless=new", "--disable-gpu", "--no-first-run",
        ...constrainedRunnerArguments,
        "--no-default-browser-check", "--remote-debugging-address=127.0.0.1",
        `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
        "--window-size=760,900", "--noerrdialogs", url], {stdio: ["ignore", "ignore", "pipe"]});
    chrome.stderr.on("data", chunk => { chromeErrors = `${chromeErrors}${chunk}`.slice(-8000); });
    devTools = await DevTools.connect(await pageTarget(port));
    const browserVersion = await devTools.call("Browser.getVersion");
    await devTools.call("Runtime.enable");
    await waitUntil(devTools, `document.querySelector(".top-actions")`, "CodexUI application shell");
    await setWidth(devTools, 760);
    await waitUntil(devTools, `document.querySelectorAll(".responsive-pane-button").length===2`, "760px drawer controls");
    assertTopBar(await devTools.evaluate(fullTopBarMeasurement), 760);
    const focus = await devTools.evaluate(`(()=>{const input=document.querySelector(".connection-control input");input.focus();
        const style=getComputedStyle(input);return [style.outlineStyle,style.outlineWidth,style.outlineColor]})()`);
    assert.deepEqual(focus, ["solid", "2px", "rgb(111, 152, 232)"]);

    const opened = await devTools.evaluate(`(async()=>{[...document.querySelectorAll(".responsive-pane-button")]
        .find(button=>button.textContent.includes("Inspector")).click();await new Promise(done=>setTimeout(done,30));
        const drawer=document.querySelector("#inspector-pane");return {focus:document.activeElement?.getAttribute("aria-label"),
        inert:[...document.querySelectorAll("[data-modal-background]")].every(element=>element.hasAttribute("inert")&&element.getAttribute("aria-hidden")==="true"),
        role:drawer?.getAttribute("role"),modal:drawer?.getAttribute("aria-modal")}})()`);
    assert.deepEqual(opened, {focus: "Close Inspector drawer", inert: true, role: "dialog", modal: "true"});
    const trapped = await devTools.evaluate(`(()=>{document.dispatchEvent(new KeyboardEvent("keydown",{key:"Tab",shiftKey:true,bubbles:true,cancelable:true}));
        const backward=document.activeElement?.textContent?.trim();document.dispatchEvent(new KeyboardEvent("keydown",{key:"Tab",bubbles:true,cancelable:true}));
        return {backward,forward:document.activeElement?.getAttribute("aria-label")}})()`);
    assert.deepEqual(trapped, {backward: "Protocol", forward: "Close Inspector drawer"});
    const closed = await devTools.evaluate(`(async()=>{document.dispatchEvent(new KeyboardEvent("keydown",{key:"Escape",bubbles:true,cancelable:true}));
        await new Promise(done=>setTimeout(done,30));return {drawer:Boolean(document.querySelector("#inspector-pane")),
        focus:document.activeElement?.getAttribute("aria-controls"),inert:[...document.querySelectorAll("[data-modal-background]")].some(element=>element.hasAttribute("inert"))}})()`);
    assert.deepEqual(closed, {drawer: false, focus: "inspector-pane", inert: false});

    await devTools.evaluate(`[...document.querySelectorAll(".responsive-pane-button")].find(button=>button.textContent.includes("Threads")).click()`);
    await wait(30);
    await setWidth(devTools, 900);
    await waitUntil(devTools, `!document.querySelector("#thread-pane")&&!([...document.querySelectorAll(".responsive-pane-button")]
        .some(button=>button.textContent.includes("Threads")))`, "tablet drawer fallback");
    const fallback = await devTools.evaluate(`({drawer:Boolean(document.querySelector("#thread-pane")),
        focusTag:document.activeElement?.tagName,focusClass:document.activeElement?.className,
        threadTrigger:[...document.querySelectorAll(".responsive-pane-button")].some(button=>button.textContent.includes("Threads")),
        overflow:document.documentElement.scrollWidth-document.documentElement.clientWidth,
        width:innerWidth,mobile:matchMedia("(max-width: 760px)").matches})`);
    assert.deepEqual(fallback, {drawer: false, focusTag: "MAIN", focusClass: "conversation-pane", threadTrigger: false,
        overflow: 0, width: 900, mobile: false});

    for (const width of [521, 360]) {
        await setWidth(devTools, width);
        await waitUntil(devTools, `document.querySelectorAll(".responsive-pane-button").length===2`, `${width}px drawer controls`);
        const measurement = await devTools.evaluate(fullTopBarMeasurement);
        assertTopBar(measurement, width);
        assert.deepEqual(measurement.drawers, ["☰ Threads", "ⓘ Inspector"]);
    }
    await devTools.call("Emulation.setTouchEmulationEnabled", {enabled: true, maxTouchPoints: 1});
    await waitUntil(devTools, `matchMedia("(pointer: coarse)").matches`, "coarse-pointer media query");
    const touch = await devTools.evaluate(`({coarse:matchMedia("(pointer: coarse)").matches,
        paneHeight:document.querySelector(".responsive-pane-button").getBoundingClientRect().height,
        viewHeight:document.querySelector(".conversation-view-controls button").getBoundingClientRect().height})`);
    assert.equal(touch.coarse, true);
    assert(touch.paneHeight >= 44 && touch.viewHeight >= 44);

    await devTools.call("Emulation.setTouchEmulationEnabled", {enabled: false});
    await setWidth(devTools, 1280);
    await waitUntil(devTools, `document.querySelector(".thread-list")&&document.querySelector(".inspector-pane")`,
        "desktop application profile layout");
    await devTools.call("Performance.enable");
    const setup = await devTools.evaluate(applicationProfileSetup);
    assert.equal(setup.resumeRequests, 1, "the application profile starts with one metadata-only resume");
    assert.deepEqual(setup.resumeParams, {threadId: "profile", excludeTurns: true});
    assert.equal(setup.catalogUpdated, true,
        "an unchanged settings draft reprojects refreshed model and permission catalogs");
    const exactInteraction = await devTools.evaluate(exactInteractionLifetimeCheck);
    assert.deepEqual(exactInteraction, {
        composerImmediate: "retain authored text", composerSettled: "retain authored text",
        turnStartsBefore: 0, turnStartsAfter: 0, rowRetained: true, focusRetained: true,
        levelBefore: "2", levelAfter: "1", menuFocused: true, menuClosed: true, focusReturned: true,
    });

    if (process.env.CODEXUI_BROWSER_CPU_PROFILE) {
        await devTools.call("Profiler.enable");
        await devTools.call("Profiler.setSamplingInterval", {interval: 100});
        await devTools.call("Profiler.start");
    }
    const beforeHydrate = await performanceSnapshot(devTools);
    const hydrateResult = await devTools.evaluate(applicationProfileHydrate);
    const afterHydrate = await performanceSnapshot(devTools);
    if (process.env.CODEXUI_BROWSER_CPU_PROFILE) {
        const {profile} = await devTools.call("Profiler.stop");
        await writeFile(`${process.env.CODEXUI_BROWSER_CPU_PROFILE}.hydrate`, JSON.stringify(profile));
    }
    const hydratePerformance = performanceDelta(beforeHydrate, afterHydrate);
    assert.deepEqual({items: hydrateResult.authoritativeItems, cards: hydrateResult.visibleCards,
        history: hydrateResult.hasHistoryBoundary}, {items: 10_000, cards: 80, history: true});
    assert.deepEqual({turns: hydrateResult.turnPages, items: hydrateResult.itemPages}, {turns: 1, items: 160});
    checkTiming(hydrateResult.wallMilliseconds <= applicationPerformanceLimits.hydrateWallMilliseconds
        && hydratePerformance.taskMilliseconds <= applicationPerformanceLimits.hydrateTaskMilliseconds,
        `10k App hydration exceeded its gate: ${hydrateResult.wallMilliseconds}/${hydratePerformance.taskMilliseconds} ms`);
    checkPerformance(hydratePerformance.layouts <= 2 && hydratePerformance.styleRecalculations <= 2,
        `10k App hydration caused ${hydratePerformance.layouts} layouts and ${hydratePerformance.styleRecalculations} style passes`);

    const prepareResult = await devTools.evaluate(applicationProfilePrepare);
    assert.equal(prepareResult.settingsControls, 12);
    const beforeIdle = await performanceSnapshot(devTools);
    // This is the measured idle interval, not a warm-up: do not drive browser work to observe inactivity.
    await wait(1000 / 30);
    const idlePerformance = performanceDelta(beforeIdle, await performanceSnapshot(devTools));
    checkTiming(idlePerformance.taskMilliseconds <= applicationPerformanceLimits.idleTaskMilliseconds,
        `idle App exceeded its task gate: ${idlePerformance.taskMilliseconds} ms`);
    checkPerformance(idlePerformance.layouts === 0 && idlePerformance.styleRecalculations === 0,
        `idle App exceeded its zero-work gate: ${idlePerformance.layouts}/${idlePerformance.styleRecalculations} passes`);
    const beforeNoOp = await performanceSnapshot(devTools);
    const noOpResult = await devTools.evaluate(applicationProfileNoOp);
    const afterNoOp = await performanceSnapshot(devTools);
    const noOpPerformance = performanceDelta(beforeNoOp, afterNoOp);
    assert(noOpResult.gridStable && noOpResult.controlsStable && noOpResult.optionsStable,
        "a semantic settings no-op replaced its visual objects");
    assert(noOpResult.focusStable && noOpResult.scrollStable
        && noOpResult.activityBefore === noOpResult.activityAfter,
    "a semantic settings no-op changed focus, scroll, or displayed activity");
    assert.equal(noOpResult.settingsStructuralMutations, 0);
    assert.equal(noOpResult.conversationStructuralMutations, 0);
    checkPerformance(noOpPerformance.layouts === 0, "a semantic settings no-op caused layout");
    checkPerformance(noOpPerformance.styleRecalculations === 0, "a semantic settings no-op caused style recalculation");
    checkTiming(noOpPerformance.taskMilliseconds <= applicationPerformanceLimits.semanticNoOpTaskMilliseconds,
        `semantic settings no-op exceeded its task gate: ${noOpPerformance.taskMilliseconds} ms`);

    if (process.env.CODEXUI_BROWSER_CPU_PROFILE) {
        await devTools.call("Profiler.enable");
        await devTools.call("Profiler.setSamplingInterval", {interval: 100});
        await devTools.call("Profiler.start");
    }
    const beforeStream = await performanceSnapshot(devTools);
    const streamResult = await devTools.evaluate(applicationProfileStream);
    const afterStream = await performanceSnapshot(devTools);
    if (process.env.CODEXUI_BROWSER_CPU_PROFILE) {
        const {profile} = await devTools.call("Profiler.stop");
        await writeFile(process.env.CODEXUI_BROWSER_CPU_PROFILE, JSON.stringify(profile));
    }
    const streamPerformance = performanceDelta(beforeStream, afterStream);
    assert.equal(streamResult.streamedDeltas, 2_000);
    assert.equal(streamResult.textGrowth, 2_000);
    assert(streamResult.targetCardStable && streamResult.targetTextStable && streamResult.cardsStable,
        "streaming replaced the visible card or Markdown text object");
    assert(streamResult.controlsStable && streamResult.optionsStable && streamResult.focusStable,
        "conversation streaming replaced unchanged settings controls");
    assert(streamResult.followingGapBefore <= 1 && streamResult.followingGapAfter <= 1,
        `following-tail drifted from ${streamResult.followingGapBefore} to ${streamResult.followingGapAfter}px`);
    assert.equal(streamResult.settingsStructuralMutations, 0);
    assert.equal(streamResult.conversationStructuralMutations, 0);
    assert(streamResult.targetTextMutations <= 1,
        `2,000 coalesced deltas caused ${streamResult.targetTextMutations} text mutations`);
    checkTiming(streamResult.ingestMilliseconds <= applicationPerformanceLimits.streamIngestMilliseconds
        && streamResult.settledMilliseconds <= applicationPerformanceLimits.streamSettledMilliseconds
        && streamPerformance.taskMilliseconds <= applicationPerformanceLimits.streamTaskMilliseconds,
    `2k App stream exceeded its gate: ${streamResult.ingestMilliseconds}/${streamResult.settledMilliseconds}/${streamPerformance.taskMilliseconds} ms`);
    checkPerformance(streamPerformance.layouts <= 2 && streamPerformance.styleRecalculations <= 2,
        `2k App stream caused ${streamPerformance.layouts} layouts and ${streamPerformance.styleRecalculations} style passes`);

    const detachedResult = await devTools.evaluate(applicationProfileDetachedStream);
    assert(detachedResult.detachedBefore > 24 && detachedResult.detachedAfter > 24,
        "a manually detached viewport resumed following-tail");
    assert(detachedResult.anchorStable && detachedResult.anchorTopDelta <= 1 && detachedResult.sourceGrowth === 2_700,
        `paused anchor moved ${detachedResult.anchorTopDelta}px across an above-anchor stream update`);

    const settingsDom = await devTools.evaluate(applicationSettingsDomContract);
    assert.equal(settingsDom.labelsAssociated, true, "every settings control has one visible label association");
    assert.deepEqual({model: settingsDom.modelDescription, profile: settingsDom.profileDescription},
        {model: "Primary model", profile: "Workspace access"});
    assert.deepEqual(settingsDom.unknownApproval, {value: "future-policy", label: "Future policy"});
    assert.deepEqual(settingsDom.unsupportedStyle,
        {disabled: true, title: "The selected model does not support style choices"});
    assert.deepEqual(settingsDom.fullAccessNetwork,
        {value: "enabled", disabled: true, title: "Full access already includes network access"});
    assert.deepEqual(settingsDom.profileSelected,
        {value: ":workspace", access: "default", network: "default"});
    assert.match(settingsDom.changedText, /12 changed/u);
    const {threadId, clientUserMessageId, input, ...settingsParams} = settingsDom.params;
    assert.equal(threadId, "profile");
    assert.equal(typeof clientUserMessageId, "string");
    assert.deepEqual(input, [{type: "text", text: "settings DOM contract", text_elements: []}]);
    assert.deepEqual(settingsParams, {
        model: "gpt-a", effort: "low", personality: "friendly", approvalPolicy: "never",
        approvalsReviewer: "auto_review", serviceTier: "fast", summary: "concise", cwd: "/authored/browser",
        permissions: null,
        sandboxPolicy: {type: "workspaceWrite", writableRoots: [], networkAccess: true,
            excludeTmpdirEnvVar: false, excludeSlashTmp: false},
        collaborationMode: {mode: "plan", settings: {model: "gpt-a", developer_instructions: null,
            reasoning_effort: "low"}},
    });

    console.log(JSON.stringify({browser: browserVersion.product, widths: [760, 521, 360], overflow: 0,
        drawerFocus: "qualified", focusTrap: "qualified", breakpointFallback: "qualified", coarseTargets: "44px",
        appPerformance: {limits: applicationPerformanceLimits, hydrate: {...hydrateResult, ...hydratePerformance},
            idle: idlePerformance, semanticNoOp: {...noOpResult, ...noOpPerformance},
            stream: {...streamResult, ...streamPerformance},
            detachedStream: detachedResult,
            nodes: afterStream.Nodes, jsHeapBytes: afterStream.JSHeapUsedSize}}, null, 2));
    assert.deepEqual(performanceFailures, [], "Browser performance qualification failed");
} catch (error) {
    if (chromeErrors) process.stderr.write(chromeErrors);
    throw error;
} finally {
    devTools?.close();
    chrome?.kill("SIGTERM");
    if (chrome && chrome.exitCode === null)
        await Promise.race([new Promise(complete => chrome.once("exit", complete)), wait(1000)]);
    if (chrome?.exitCode === null) {
        chrome.kill("SIGKILL");
        await Promise.race([new Promise(complete => chrome.once("exit", complete)), wait(1000)]);
    }
    await new Promise(complete => server.close(complete));
    await rm(profile, {recursive: true, force: true, maxRetries: 5, retryDelay: 100});
}
