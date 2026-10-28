import assert from "node:assert/strict";
import {mkdir, writeFile} from "node:fs/promises";
import {join} from "node:path";

export async function qualifyCatchup(devTools) {
    const result = await devTools.evaluate(`(async()=>{
        const socket=globalThis.codexuiApplicationProfile.socket;
        const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));
        const waitFor=async(predicate,label)=>{for(let i=0;i<200;i++){if(predicate())return;await new Promise(done=>setTimeout(done,5));}throw new Error("Catchup: "+label);};
        const check=(condition,label)=>{if(!condition)throw new Error("Catchup: "+label);};
        const button=(text,root=document)=>[...root.querySelectorAll("button")].find(button=>button.textContent.trim()===text);
        const set=(element,value)=>{Object.getOwnPropertyDescriptor(element instanceof HTMLTextAreaElement?HTMLTextAreaElement.prototype:element instanceof HTMLSelectElement?HTMLSelectElement.prototype:HTMLInputElement.prototype,"value").set.call(element,value);element.dispatchEvent(new Event(element instanceof HTMLSelectElement?"change":"input",{bubbles:true}));};
        const notify=(method,params)=>socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",method,params}});
        const projects=new Map([["p",{id:"p",name:"Project Alpha",position:0,roots:[{path:"/workspace/alpha"}],metadata:{foreign:"preserved","codexui.description":"Initial description"}}]]);
        const sections=new Map([["s",{id:"s",name:"Review"}],["empty",{id:"empty",name:"Empty section"}]]);
        const threads=new Map([["profile",{id:"profile",name:"Profile thread",cwd:"/workspace/alpha",projectId:"p",section:{id:"s",name:"Review"},createdAt:100,updatedAt:120,status:{type:"idle"}}],
            ["archive",{id:"archive",name:"Archived thread",cwd:"/workspace/alpha",archived:true,status:{type:"idle"}}]]);
        const prior=socket.send.bind(socket);let sequence=0;
        socket.send=function(data){const message=JSON.parse(data),payload=message.kind==="appserver"?message.payload:null;
            if(!payload?.method)return prior(data);
            const p=payload.params??{};let result;
            switch(payload.method){
                case "project/list":result={data:[...projects.values()],nextCursor:null};break;
                case "threadSection/list":result={data:[...sections.values()],nextCursor:null};break;
                case "project/create":{const group={id:"new-project",...p};projects.set(group.id,group);result={project:group};break;}
                case "project/read":result={project:projects.get(p.projectId)};break;
                case "project/update":{const group={...projects.get(p.projectId),...p};projects.set(p.projectId,group);result={project:group};break;}
                case "threadSection/create":{const group={id:"new-section",...p};sections.set(group.id,group);result={section:group};break;}
                case "threadSection/update":{const group={...sections.get(p.sectionId),...p};sections.set(p.sectionId,group);result={section:group};break;}
                case "thread/list":result={data:[...threads.values()].filter(thread=>!!thread.archived===!!p.archived&&(!p.projectId||thread.projectId===p.projectId)&&(!p.sectionId||thread.section?.id===p.sectionId)&&(!p.searchTerm||thread.name.includes(p.searchTerm))),nextCursor:null};break;
                case "thread/read":case "thread/resume":result={thread:threads.get(p.threadId)??{id:p.threadId}};break;
                case "thread/turns/list":result={data:[],nextCursor:null};break;
                case "thread/start":{const thread={id:"created-"+(++sequence),cwd:p.cwd,projectId:p.projectId??null,status:{type:"idle"},name:"Created thread"};threads.set(thread.id,thread);result={thread};break;}
                case "thread/name/set":{const thread=threads.get(p.threadId);if(thread)thread.name=p.name;result={};queueMicrotask(()=>notify("thread/name/updated",{threadId:p.threadId,threadName:p.name}));break;}
                case "thread/section/move":{const thread=threads.get(p.threadId);if(thread)thread.section=p.sectionId?sections.get(p.sectionId):null;result={};break;}
                case "thread/metadata/update":{const thread=threads.get(p.threadId);if(thread)thread.projectId=p.projectId;result={thread};break;}
                case "turn/start":if(this.rejectNextTurn){this.rejectNextTurn=false;this.sent.push(message);queueMicrotask(()=>this.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:payload.id,error:{code:-32000,message:"Rejected catchup prompt"}}}));return;}result={turn:{id:"catchup-turn-"+(++sequence),status:"inProgress"}};break;
                default:return prior(data);
            }
            this.sent.push(message);queueMicrotask(()=>this.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:payload.id,result}}));
        };
        notify("thread/started",{thread:threads.get("profile")});document.querySelector(".thread-browser-menu > button").click();button("Refresh").click();
        await waitFor(()=>document.querySelectorAll(".browser-group").length>=3,"group catalogs");await frame();
        check(document.querySelector(".thread-list").textContent.includes("Empty section"),"empty section discoverable");
        check([...document.querySelectorAll(".thread-title")].filter(node=>node.textContent==="Profile thread").length===1,"thread not duplicated");
        // Project edit goes through a fresh read, preserving another client's metadata.
        const project=[...document.querySelectorAll(".browser-group")].find(group=>group.querySelector(":scope > header .group-disclosure")?.textContent.includes("Project Alpha"));
        const groupTrigger=project.querySelector(":scope > header .group-menu-trigger");
        project.querySelector(":scope > header").dispatchEvent(new MouseEvent("contextmenu",{clientX:innerWidth-2,clientY:innerHeight-2,bubbles:true,cancelable:true}));await frame();
        const menuBounds=document.querySelector(".thread-context-menu").getBoundingClientRect();
        check(menuBounds.left>=0&&menuBounds.right<=innerWidth&&menuBounds.top>=0&&menuBounds.bottom<=innerHeight,"group menu remains in viewport near an edge");
        document.dispatchEvent(new KeyboardEvent("keydown",{key:"Escape",bubbles:true,cancelable:true}));await frame();
        check(document.activeElement===project.querySelector(":scope > header .group-disclosure"),"right-click menu returns focus to visible group control");
        groupTrigger.click();await frame();
        button("Edit…",document.querySelector(".thread-context-menu")).click();await waitFor(()=>document.querySelector("dialog[open]"),"project edit dialog");
        const dialog=document.querySelector("dialog[open]");
        set([...dialog.querySelectorAll("label")].find(label=>label.textContent.startsWith("Name")).querySelector("input"),"Project Renamed");
        set([...dialog.querySelectorAll("label")].find(label=>label.textContent.startsWith("Description")).querySelector("textarea"),"Updated description");
        button("Save",dialog).click();await waitFor(()=>!document.querySelector("dialog[open]"),"project saved");
        check(projects.get("p").metadata.foreign==="preserved"&&projects.get("p").metadata["codexui.description"]==="Updated description","foreign project metadata retained");
        document.querySelector(".thread-browser-menu > button").click();button("New section…").click();await waitFor(()=>document.querySelector("dialog[open]"),"section create dialog");
        const sectionDialog=document.querySelector("dialog[open]");set(sectionDialog.querySelector("input"),"New empty section");button("Save",sectionDialog).click();
        await waitFor(()=>!document.querySelector("dialog[open]")&&document.querySelector(".thread-list").textContent.includes("New empty section"),"section created");
        const archive=document.querySelector('[aria-label="Thread archive filter"]');set(archive,"archived");
        await waitFor(()=>[...document.querySelectorAll(".thread-title")].some(node=>node.textContent==="Archived thread"),"archived browser query");
        check(![...document.querySelectorAll(".thread-title")].some(node=>node.textContent==="Profile thread"),"active hidden by archive filter");
        set(archive,"active");const search=document.querySelector('[aria-label="Search thread titles"]');set(search,"No match");
        search.dispatchEvent(new KeyboardEvent("keydown",{key:"Enter",bubbles:true}));await frame();
        check(document.querySelectorAll(".thread-row").length===0,"search filters across retained entities");set(search,"");await frame();
        // Standalone creation must never silently inherit the selected thread path.
        document.querySelector(".new-thread-button").click();await frame();
        check(document.querySelector(".new-thread-dialog input").value==="","standalone workspace requires an explicit choice");
        button("Cancel",document.querySelector(".new-thread-dialog")).click();await frame();
        // Multiple project roots have no authoritative default.
        projects.get("p").roots.push({path:"/workspace/beta"});notify("project/changed",{projectId:"p",changeType:"updated"});await frame();
        const multipleSection=[...document.querySelectorAll(".browser-group.nested-section")].find(group=>group.textContent.includes("Review"));
        multipleSection.querySelector(":scope > header .group-menu-trigger").click();await frame();
        button("New thread here…",document.querySelector(".thread-context-menu")).click();await frame();
        check(document.querySelector(".new-thread-dialog input").value===""&&document.querySelectorAll("#workspace-roots option").length===2,"multiple roots require an explicit root choice");
        button("Cancel",document.querySelector(".new-thread-dialog")).click();await frame();
        projects.get("p").roots.pop();notify("project/changed",{projectId:"p",changeType:"updated"});await frame();
        // Grouped draft inherits both project workspace and section identity.
        const nested=[...document.querySelectorAll(".browser-group.nested-section")].find(group=>group.textContent.includes("Review"));
        nested.querySelector(":scope > header .group-menu-trigger").click();await frame();
        button("New thread here…",document.querySelector(".thread-context-menu")).click();await waitFor(()=>document.querySelector(".modal-backdrop"),"grouped thread dialog");
        check(document.querySelector(".new-thread-dialog input").value==="/workspace/alpha","project root inherited inside section");
        set(document.querySelector(".new-thread-dialog input"),"/workspace/explicit-override");await frame();
        button("Continue",document.querySelector(".modal-backdrop")).click();await frame();
        const editor=document.querySelector('[aria-label="Message Codex"]');set(editor,"grouped prompt");button("Send").click();
        await waitFor(()=>socket.sent.some(message=>message.payload?.method==="thread/section/move"),"section assignment after creation");await frame();
        const created=[...threads.values()].find(thread=>thread.id.startsWith("created-"));
        check(created.projectId==="p"&&created.section.id==="s"&&created.cwd==="/workspace/explicit-override","group context and deliberate workspace override reach provider");
        // Live token and timing facts are projected through the production application.
        notify("thread/tokenUsage/updated",{threadId:created.id,tokenUsage:{total:{totalTokens:1200,inputTokens:1000,outputTokens:200},last:{totalTokens:50},modelContextWindow:1000}});
        notify("turn/started",{threadId:created.id,turn:{id:"visible-turn",status:"inProgress",startedAt:100}});
        notify("item/started",{threadId:created.id,turnId:"visible-turn",startedAtMs:100001,item:{id:"command",type:"commandExecution",command:Array.from({length:50},(_,i)=>"command "+i).join("\\n"),aggregatedOutput:Array.from({length:80},(_,i)=>"output "+i).join("\\n"),status:"inProgress"}});
        await waitFor(()=>document.querySelector(".command-output"),"command card");await frame();
        check(document.querySelector(".conversation-heading").textContent.includes("Left ≈950"),"token/context display");
        const command=document.querySelector(".command-line"),output=document.querySelector(".command-output");
        check(command.scrollTop===0&&output.scrollTop>0,"command begins at top; output follows tail");
        output.scrollTop=35;output.dispatchEvent(new Event("scroll",{bubbles:true}));await frame();
        const before=output.scrollTop,card=output.closest(".conversation-card");card.querySelector('[aria-label="Collapse card"]').click();await frame();
        card.querySelector('[aria-label="Expand card"]').click();await frame();
        check(Math.abs(card.querySelector(".command-output").scrollTop-before)<=1,"output detach restored after folding");
        card.querySelector('[aria-label="Timing details"]').click();await frame();
        check(document.querySelector(".inspector-content").textContent.includes("100001 Unix milliseconds"),"card timing routes to Info details");
        // Paused output also survives thread switches, because the session owns it.
        notify("item/commandExecution/outputDelta",{threadId:created.id,turnId:"visible-turn",itemId:"command",delta:"\\nmore"});await frame();
        check(Math.abs(card.querySelector(".command-output").scrollTop-before)<=1,"stream preserves detached output");
        notify("item/started",{threadId:created.id,turnId:"visible-turn",item:{id:"agent",type:"collabAgentToolCall",tool:"spawnAgent",model:"gpt-a",reasoningEffort:"high",prompt:"Agent prompt",resultText:"**Agent result**",receiverThreadIds:["child"],status:"completed"}});
        await frame();button("Agents",document.querySelector(".inspector-pane")).click();await frame();
        const fold=document.querySelector('[aria-label="Expand agent"]');check(!!fold,"agent card available");fold.click();await frame();
        check(document.querySelector(".agent-card-content").textContent.includes("Agent prompt")&&!!document.querySelector(".agent-card-content strong"),"full agent content and Markdown");
        button("Plan",document.querySelector(".inspector-pane")).click();await frame();button("Agents",document.querySelector(".inspector-pane")).click();await frame();
        check(!!document.querySelector('[aria-label="Collapse agent"]'),"agent disclosure survives tab switches");
        // Requests stay visible beside the composer and route through current request identity.
        socket.receive({kind:"appserver",payload:{jsonrpc:"2.0",id:"attention-request",method:"item/commandExecution/requestApproval",params:{threadId:created.id,turnId:"visible-turn",itemId:"command",command:"true"}}});await frame();
        check(!!document.querySelector(".attention-strip")&&!!document.querySelector(".requests-indicator"),"request attention visible outside Inspector");
        button("Review 1 request(s)").click();await frame();check(!!document.querySelector(".request-card"),"review navigation");
        notify("serverRequest/resolved",{requestId:"attention-request"});await frame();check(!document.querySelector(".attention-strip"),"resolved attention removed");
        const center=document.querySelector(".conversation-pane");document.querySelector('[aria-label="Hide Threads"]').click();await frame();
        check(!document.querySelector(".thread-pane")&&document.querySelector(".conversation-pane")===center,"hide Threads preserves conversation");button("Show threads").click();await frame();
        const splitter=document.querySelector('[aria-label="Resize Threads"]'),width=document.querySelector(".thread-pane").getBoundingClientRect().width;
        splitter.dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowRight",bubbles:true}));await frame();
        check(document.querySelector(".thread-pane").getBoundingClientRect().width>width,"keyboard splitter resizes pane");
        check(document.documentElement.scrollWidth===document.documentElement.clientWidth,"desktop has no horizontal overflow");
        globalThis.codexuiCatchup={socket,notify,threads,created,projects};
        return {groups:true,metadata:true,search:true,archive:true,groupedCreation:true,tokens:true,timing:true,commandScroll:true,agents:true,attention:true,panes:true};
    })()`);
    assert.equal(Object.values(result).every(Boolean), true);
    const setPrompt = async value => {
        await devTools.evaluate(`(()=>{const e=document.querySelector('[aria-label="Message Codex"]');Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,"value").set.call(e,${JSON.stringify(value)});e.dispatchEvent(new Event("input",{bubbles:true}));e.focus();e.setSelectionRange(e.value.length,e.value.length);})()`);
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    };
    const promptState = () => devTools.evaluate(`(()=>{const e=document.querySelector('[aria-label="Message Codex"]');return {text:e.value,start:e.selectionStart,end:e.selectionEnd,height:e.clientHeight,content:e.scrollHeight,overflow:getComputedStyle(e).overflowY};})()`);
    const key = async (key,code,number,text="",repeat=false,modifiers=0) => {
        await devTools.call("Input.dispatchKeyEvent",{type:"keyDown",key,code,windowsVirtualKeyCode:number,...(text?{text,unmodifiedText:text}:{}),autoRepeat:repeat,modifiers});
        await devTools.call("Input.dispatchKeyEvent",{type:"keyUp",key,code,windowsVirtualKeyCode:number,modifiers});
    };
    await setPrompt("xy");await key("a","KeyA",65,"a");for(let i=0;i<3;i++)await key("a","KeyA",65,"a",true);
    assert.equal((await promptState()).text,"xyaaaa","ordinary repeated typing remains native editing");
    for(let i=0;i<3;i++)await key("Backspace","Backspace",8,"",true);
    assert.equal((await promptState()).text,"xya","repeated Backspace deletes text");
    for(let i=0;i<3;i++)await key("ArrowLeft","ArrowLeft",37,"",true);
    assert.equal((await promptState()).start,0,"repeated arrows move the caret");
    for(let i=0;i<2;i++)await key("ArrowRight","ArrowRight",39,"",true,8);
    assert.equal((await promptState()).end,2,"repeated Shift+arrow extends selection");
    await setPrompt("line");await key("Enter","Enter",13,"\r",true,8);
    assert.equal((await promptState()).text,"line\n","repeated Shift+Enter inserts a newline");
    const wireBefore = await devTools.evaluate(`globalThis.codexuiCatchup.socket.sent.length`);
    await key("Enter","Enter",13,"\r",true);
    assert.equal(await devTools.evaluate(`globalThis.codexuiCatchup.socket.sent.length`),wireBefore,"repeated submit Enter does not send");
    await devTools.call("Emulation.setDeviceMetricsOverride",{width:1280,height:900,deviceScaleFactor:1,mobile:false});
    await setPrompt("a ".repeat(200));const beforeResize=await promptState();
    await devTools.call("Emulation.setDeviceMetricsOverride",{width:360,height:900,deviceScaleFactor:1,mobile:false});
    await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    const afterResize=await promptState();
    assert.equal(afterResize.text,beforeResize.text,"resizing retains the exact draft");
    assert(afterResize.height>=afterResize.content||afterResize.overflow==="auto","resized prompt never hides wrapped content");
    assert(afterResize.height>beforeResize.height||afterResize.overflow==="auto","width changes invalidate prompt measurement");
    await setPrompt("");await devTools.call("Emulation.setDeviceMetricsOverride",{width:1280,height:900,deviceScaleFactor:1,mobile:false});
    await devTools.evaluate(`(async()=>{for(let attempt=0;attempt<200&&!document.querySelector(".thread-pane .thread-browser-menu > button");attempt++){await new Promise(done=>requestAnimationFrame(done));}if(!document.querySelector(".thread-pane .thread-browser-menu > button"))throw new Error("Desktop thread pane did not return after restoring 1280px width");})()`);
    result.promptKeyboard=true;result.promptResize=true;
    await devTools.evaluate(`document.querySelector(".connection-menu > button").focus()`);await key("Enter","Enter",13,"\r");
    await devTools.evaluate(`document.querySelector(".connection-control input").focus()`);
    assert(await devTools.evaluate(`Boolean(document.querySelector(".connection-menu .menu-popover:popover-open"))`),"connection uses a native popover");
    await key("Escape","Escape",27);
    assert(await devTools.evaluate(`!document.querySelector(":popover-open")&&document.activeElement===document.querySelector(".connection-menu > button")`),"Escape closes the connection menu and returns focus");
    await devTools.evaluate(`document.querySelector(".thread-browser-menu > button").click()`);
    const menuBounds=await devTools.evaluate(`(()=>{const r=document.querySelector(":popover-open").getBoundingClientRect();return {left:r.left,top:r.top,right:r.right,bottom:r.bottom};})()`);
    assert(menuBounds.left>=0&&menuBounds.top>=0&&menuBounds.right<=1280&&menuBounds.bottom<=900,"menu remains within the viewport");
    await devTools.call("Input.dispatchMouseEvent",{type:"mousePressed",x:40,y:32,button:"left",clickCount:1});await devTools.call("Input.dispatchMouseEvent",{type:"mouseReleased",x:40,y:32,button:"left",clickCount:1});
    assert(await devTools.evaluate(`!document.querySelector(":popover-open")`),"outside click dismisses a native menu");
    await devTools.call("Emulation.setDeviceMetricsOverride",{width:360,height:900,deviceScaleFactor:1,mobile:false});
    await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    await devTools.evaluate(`[...document.querySelectorAll(".responsive-pane-button")].find(button=>button.textContent.includes("Threads")).click()`);
    await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    await devTools.evaluate(`document.querySelector("#thread-pane .thread-browser-menu > button").focus();document.querySelector("#thread-pane .thread-browser-menu > button").click();document.querySelector("#thread-pane .thread-browser-menu select").focus()`);
    await key("Escape","Escape",27);
    assert(await devTools.evaluate(`Boolean(document.querySelector("#thread-pane"))&&!document.querySelector(":popover-open")`),"first Escape closes the menu while retaining its drawer");
    await key("Escape","Escape",27);
    assert(await devTools.evaluate(`!document.querySelector("#thread-pane")`),"second Escape closes the drawer");
    await devTools.call("Emulation.setDeviceMetricsOverride",{width:1280,height:900,deviceScaleFactor:1,mobile:false});
    result.menus=true;
    // Standalone threads exercise the outer list; grouped threads now scroll
    // inside their bounded project bodies instead.
    await devTools.evaluate(`(()=>{const {notify}=globalThis.codexuiCatchup;for(let i=0;i<200;i++)notify("thread/started",{thread:{id:"scroll-reference-"+i,name:"Scroll reference "+i,status:{type:"idle"}}});})()`);
    await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    const treeScroll=await devTools.evaluate(`(()=>{const p=document.querySelector(".thread-pane"),l=document.querySelector(".thread-list"),pr=p.getBoundingClientRect(),lr=l.getBoundingClientRect();l.scrollTop=l.scrollHeight;return {layout:getComputedStyle(p).display,height:l.clientHeight,content:l.scrollHeight,bottom:lr.bottom,paneBottom:pr.bottom,scrolled:l.scrollTop};})()`);
    assert.equal(treeScroll.layout,"flex","thread pane retains its shared flex layout");
    assert(treeScroll.height>100&&treeScroll.content>treeScroll.height&&treeScroll.scrolled>0&&treeScroll.bottom<=treeScroll.paneBottom+1,"large thread catalogs scroll inside their bounded pane");
    await devTools.evaluate(`(()=>{const {notify}=globalThis.codexuiCatchup;for(let i=0;i<200;i++)notify("thread/deleted",{threadId:"scroll-reference-"+i});document.querySelector(".thread-list").scrollTop=0;})()`);
    await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    result.threadScroll=true;


    const metrics = async width => {
        await devTools.call("Emulation.setDeviceMetricsOverride", {width, height: 900, deviceScaleFactor: 1, mobile: false});
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    };
    await metrics(2000);
    await devTools.evaluate(`(async()=>{const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));for(const label of ["Resize Threads","Resize Inspector"]){document.querySelector('[aria-label="'+label+'"]').dispatchEvent(new KeyboardEvent("keydown",{key:"End",bubbles:true}));await frame();}})()`);
    await metrics(1200);
    const resize = await devTools.evaluate(`(async()=>{const pane=document.querySelector(".thread-pane"),center=document.querySelector(".conversation-pane");const before=pane.getBoundingClientRect().width;document.querySelector('[aria-label="Resize Threads"]').dispatchEvent(new KeyboardEvent("keydown",{key:"ArrowRight",bubbles:true}));await new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));return {before,after:pane.getBoundingClientRect().width,center:center.getBoundingClientRect().width,overflow:document.documentElement.scrollWidth-innerWidth};})()`);
    assert(resize.center >= 420 && resize.overflow === 0 && Math.abs(resize.after - resize.before - 10) <= 1, "pane resizing uses visible geometry after window shrink");
    await metrics(1280);
    await devTools.evaluate(`(async()=>{const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));for(const [label,key,count] of [["Resize Threads","ArrowRight",10],["Resize Inspector","ArrowLeft",22]]){const handle=document.querySelector('[aria-label="'+label+'"]');handle.dispatchEvent(new KeyboardEvent("keydown",{key:"Home",bubbles:true}));await frame();for(let index=0;index<count;index++){handle.dispatchEvent(new KeyboardEvent("keydown",{key,bubbles:true}));await frame();}}})()`);
    const roleControls = await devTools.evaluate(`document.querySelectorAll(".top-actions > .subtle-button").length`);
    assert.equal(roleControls, 1, "one controller action in the production shell");
    if (process.env.CODEXUI_BROWSER_SCREENSHOTS) {
        await mkdir(process.env.CODEXUI_BROWSER_SCREENSHOTS, {recursive: true});
        const capture = async name => {
            await devTools.evaluate(`Promise.allSettled(document.getAnimations().filter(animation=>animation.playState==="running"&&Number.isFinite(animation.effect.getComputedTiming().endTime)).map(animation=>animation.finished))`);
            const screenshot = await devTools.call("Page.captureScreenshot", {format: "png"});
            await writeFile(join(process.env.CODEXUI_BROWSER_SCREENSHOTS, name), Buffer.from(screenshot.data, "base64"));
        };
        await devTools.evaluate(`document.querySelector('.commandExecution [aria-label="Timing details"]').click()`);
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
        await capture("catchup-desktop.png");
        await devTools.evaluate(`document.querySelector(".thread-browser-menu > button").click();[...document.querySelectorAll("button")].find(button=>button.textContent.trim()==="New project…").click()`);
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
        await capture("catchup-project-dialog.png");
        await devTools.evaluate(`[...document.querySelectorAll("dialog button")].find(button=>button.textContent.trim()==="Cancel").click()`);
        await devTools.call("Emulation.setDeviceMetricsOverride", {width: 360, height: 900, deviceScaleFactor: 1, mobile: false});
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
        assert.equal(await devTools.evaluate(`document.documentElement.scrollWidth-document.documentElement.clientWidth`), 0);
        await capture("catchup-mobile.png");
        await devTools.evaluate(`[...document.querySelectorAll(".responsive-pane-button")].find(button=>button.textContent.includes("Threads")).click()`);
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
        await capture("catchup-mobile-threads.png");
        await devTools.call("Emulation.setDeviceMetricsOverride", {width: 1280, height: 900, deviceScaleFactor: 1, mobile: false});
        await devTools.evaluate(`new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)))`);
    }
    const recovery = await devTools.evaluate(`(async()=>{
        const {socket,notify,created}=globalThis.codexuiCatchup;
        const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));
        const set=(value)=>{const editor=document.querySelector('[aria-label="Message Codex"]');Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,"value").set.call(editor,value);editor.dispatchEvent(new Event("input",{bubbles:true}));};
        for(const id of ["visible-turn","catchup-turn-2"])notify("turn/completed",{threadId:created.id,turn:{id,status:"completed"}});
        await frame();socket.rejectNextTurn=true;set("  restore **exactly**\\n\\nthis  ");await frame();
        document.querySelector('button[type="submit"].send-button').click();await frame();
        const restore=[...document.querySelectorAll("button")].find(button=>button.textContent==="Restore to composer");
        if(!restore)throw new Error("Catchup: failed prompt recovery control");
        const wireBefore=socket.sent.filter(message=>["thread/start","turn/start"].includes(message.payload?.method)).length;
        set("Keep current draft");await frame();restore.click();await frame();
        if(document.querySelector('[aria-label="Message Codex"]').value!=="Keep current draft")throw new Error("Catchup: recovery overwrote a draft");
        set("");await frame();restore.click();await frame();
        const editor=document.querySelector('[aria-label="Message Codex"]');
        if(editor.value!=="  restore **exactly**\\n\\nthis  "||document.activeElement!==editor)throw new Error("Catchup: exact prompt/focus recovery");
        if(socket.sent.filter(message=>["thread/start","turn/start"].includes(message.payload?.method)).length!==wireBefore)throw new Error("Catchup: recovery resent prompt");
        const group=[...document.querySelectorAll(".browser-group")].find(group=>group.querySelector(":scope > header .group-disclosure")?.textContent.includes("Project Renamed"));
        group.querySelector(":scope > header .group-menu-trigger").click();await frame();
        globalThis.codexuiCatchup.projects.delete("p");notify("project/changed",{projectId:"p",changeType:"deleted"});await frame();
        if(document.querySelector(".thread-context-menu"))throw new Error("Catchup: deleted group retains an actionable menu");
        return true;
    })()`);
    assert.equal(recovery, true); result.recovery = true;
    return result;
}
