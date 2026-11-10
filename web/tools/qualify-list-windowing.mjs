import assert from "node:assert/strict";

export async function qualifyListWindowing(devTools) {
    const result = await devTools.evaluate(`(async () => {
        const socket = globalThis.codexuiApplicationProfile.socket, original = socket.send.bind(socket);
        const frame = () => new Promise(done => requestAnimationFrame(() => requestAnimationFrame(done)));
        const waitFor = async (test, label) => { for (let n = 0; n < 300 && !test(); ++n) { await new Promise(done => setTimeout(done, 5)); } if (!test()) { throw new Error(label); } };
        const notify = (method, params) => socket.receive({kind: 'appserver', payload: {method, params}});
        const key = async (node, key, shiftKey = false) => { node.dispatchEvent(new KeyboardEvent('keydown', {key, shiftKey, bubbles: true, cancelable: true})); await frame(); };
        const rows = Array.from({length: 10000}, (_, i) => ({id: 'window-thread-' + i, name: 'Window thread ' + i, lineageKnown: true, parentThreadId: null, forkedFromId: null, status: {type: 'idle'}, turns: []}));
        socket.send = data => { const e = JSON.parse(data), r = e.payload; if (e.kind === 'appserver' && r?.method === 'thread/list') { socket.sent.push(e); queueMicrotask(() => socket.receive({kind: 'appserver', payload: {jsonrpc: '2.0', id: r.id, result: {data: rows, nextCursor: null}}})); } else { original(data); } };
        document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', bubbles: true})); await frame();
        document.querySelector('.thread-browser-menu > button').click(); await frame();
        const group = document.querySelector('[aria-label="Thread grouping"]'); group.value = 'ungrouped';
        let start = performance.now(); group.dispatchEvent(new Event('change', {bubbles: true})); await frame();
        document.querySelector('.thread-browser-controls button').click(); await frame();
        await waitFor(() => Number(document.querySelector('.thread-list .scroll-window')?.dataset.windowCount) >= 10000, 'logical thread inventory');
        await frame(); const hydrate = performance.now() - start;
        document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape', bubbles: true})); await frame();
        const list = document.querySelector('.thread-list'), first = list.querySelector('.thread-row');
        const logical = Number(list.querySelector('.scroll-window').dataset.windowCount);
        first.focus(); await frame(); start = performance.now(); await key(first, 'End');
        const endMs = performance.now() - start, lastPosition = Number(document.activeElement.closest('[role=treeitem]').getAttribute('aria-posinset'));
        const lastSetSize = Number(document.activeElement.closest('[role=treeitem]').getAttribute('aria-setsize'));
        const endVisible = document.activeElement.getBoundingClientRect().bottom <= list.getBoundingClientRect().bottom + 1;
        await key(document.activeElement, 'Home');
        const visited = new Set([document.activeElement.closest('[data-window-key]').dataset.windowKey]);
        for (let n = 0; n < 55; ++n) { await key(document.activeElement, 'ArrowDown'); visited.add(document.activeElement.closest('[data-window-key]').dataset.windowKey); }
        const arrowPosition = Number(document.activeElement.closest('[role=treeitem]').getAttribute('aria-posinset'));
        const focused = document.activeElement; list.scrollTop = list.scrollHeight; await frame();
        const focusRetained = focused.isConnected && document.activeElement === focused;
        focused.closest('.thread-row-wrap').querySelector('.thread-menu-trigger').click(); await frame();
        list.scrollTop = list.scrollHeight / 3; await frame();
        const menuRetained = focused.isConnected && !!document.querySelector('[role=menu]');
        document.dispatchEvent(new KeyboardEvent('keydown', {key:'Escape',bubbles:true})); await frame();
        const menuFocusRestored = document.activeElement === focused.closest('.thread-row-wrap').querySelector('.thread-menu-trigger');
        list.tabIndex = -1; list.focus(); await frame(); list.scrollTop = list.scrollHeight / 2; await frame(); await frame();
        const visible = [...list.querySelectorAll('.thread-row')].find(node => node.getBoundingClientRect().top >= list.getBoundingClientRect().top);
        const anchorTitle = visible.textContent, anchorY = visible.getBoundingClientRect().top;
        notify('thread/started', {thread: {id: 'window-inserted', name: 'Inserted newest', createdAt: 9999999999, updatedAt: 9999999999, lineageKnown: true, parentThreadId: null, forkedFromId: null, status: {type: 'idle'}, turns: []}}); await frame(); await frame();
        const retainedAnchor = [...list.querySelectorAll('.thread-row')].find(node => node.textContent === anchorTitle);
        const anchorDelta = retainedAnchor ? Math.abs(retainedAnchor.getBoundingClientRect().top - anchorY) : null;
        const threads = {logical, hydrate, endMs, lastPosition, lastSetSize, endVisible, arrowPosition, visited: visited.size, focusRetained, menuRetained, menuFocusRestored, anchorDelta, resident: list.querySelectorAll('.thread-row').length, dom: document.querySelectorAll('*').length};
        socket.send = original;
        const items = Array.from({length: 10000}, (_, i) => ({id: 'window-agent-' + i, type: 'subAgentActivity', kind: 'started', agentPath: '/root/window-agent-' + i, model: 'test', prompt: 'Prompt ' + i, resultText: 'Variable height result ' + i + '\\n\\n' + ('A paragraph of agent output.\\n\\n').repeat(i % 8 + 2)}));
        start = performance.now(); notify('turn/started', {threadId: 'menu-source', turn: {id: 'window-agent-turn', status: 'completed', items}}); await frame();
        const ingest = performance.now() - start; start = performance.now();
        [...document.querySelectorAll('.inspector-tabs button')].find(button => button.textContent === 'Agents').click(); await frame(); await frame();
        const switchMs = performance.now() - start, pane = document.querySelector('.inspector-content');
        const agentLogical = Number(pane.querySelector('.scroll-window').dataset.windowCount);
        const firstCard = pane.querySelector('.agent-card'), firstKey = firstCard.parentElement.dataset.windowKey;
        firstCard.querySelector('.agent-fold-button').click(); await frame();
        const expandedHeight = firstCard.getBoundingClientRect().height;
        pane.tabIndex = -1; pane.focus(); await frame(); pane.scrollTop = pane.scrollHeight; await frame(); await frame();
        const lastAgentPosition = Number([...pane.querySelectorAll('[role=listitem]')].at(-1).getAttribute('aria-posinset'));
        const recycled = !firstCard.isConnected;
        pane.scrollTop = 0; await frame(); await frame();
        const restored = [...pane.querySelectorAll('.agent-card')].find(card => card.parentElement.dataset.windowKey === firstKey);
        const expansionRetained = restored?.classList.contains('expanded');
        const text = restored.querySelector('.card-text').firstChild, range = document.createRange(); range.selectNodeContents(text);
        getSelection().removeAllRanges(); getSelection().addRange(range); await frame();
        pane.scrollTop = pane.scrollHeight / 2; await frame();
        const selectionRetained = restored.isConnected && getSelection().toString().startsWith('Prompt');
        const anchored = [...pane.querySelectorAll('.agent-card')].find(card => card.getBoundingClientRect().top >= pane.getBoundingClientRect().top);
        const anchoredY = anchored.getBoundingClientRect().top;
        restored.querySelector('.agent-fold-button').click(); await frame(); await frame();
        const reflowAnchorDelta = Math.abs(anchored.getBoundingClientRect().top - anchoredY);
        getSelection().removeAllRanges(); await frame();
        const tail = [...pane.querySelectorAll('.agent-fold-button')].at(-1); tail.focus(); await frame();
        const position = Number(tail.closest('[role=listitem]').getAttribute('aria-posinset'));
        await key(tail, 'Tab');
        const tabPosition = Number(document.activeElement.closest('[role=listitem]')?.getAttribute('aria-posinset'));
        const agents = {logical: agentLogical, ingest, switchMs, lastAgentPosition, recycled, expansionRetained, expandedHeight, selectionRetained, reflowAnchorDelta, position, tabPosition, resident: pane.querySelectorAll('.agent-card').length, dom: document.querySelectorAll('*').length};
        globalThis.windowingQualification = {pane, list};
        return {threads, agents};
    })()`);
    console.log(JSON.stringify({listWindowingMeasurements:result}));
    assert(result.threads.logical >= 10000);
    assert.equal(result.threads.lastPosition, result.threads.lastSetSize, "End reaches the logical last thread");
    assert.equal(result.threads.endVisible, true);
    assert.equal(result.threads.visited, 56, "each arrow reaches the next distinct logical row, including children");
    assert(result.threads.arrowPosition > 40, "arrows cross the initial residency boundary");
    assert.equal(result.threads.focusRetained, true);
    assert.equal(result.threads.menuRetained, true);
    assert.equal(result.threads.menuFocusRestored, true);
    assert(result.threads.anchorDelta !== null && result.threads.anchorDelta <= 1, "updates preserve the visible anchor");
    assert(result.threads.resident < 100, "thread residency is viewport bounded");
    assert.equal(result.agents.logical, 10000);
    assert.equal(result.agents.lastAgentPosition, 10000, "scroll reaches the logical last agent");
    assert.equal(result.agents.recycled, true);
    assert.equal(result.agents.expansionRetained, true);
    assert.equal(result.agents.selectionRetained, true);
    assert(result.agents.reflowAnchorDelta <= 1, "offscreen expansion changes preserve the visible anchor");
    assert.equal(result.agents.tabPosition, result.agents.position + 1);
    assert(result.agents.resident < 100, "agent residency is viewport bounded");
    assert(result.agents.dom < 10000, "combined document residency stays bounded");
    assert(result.threads.hydrate < 1733.8, "hydration improves on the recorded local baseline");
    assert(result.agents.switchMs < 1135.1, "Agents switching improves on the recorded local baseline");
    const geometry = [];
    for (const deviceScaleFactor of [1, 1.25, 1.5, 2]) {
        await devTools.call("Emulation.setDeviceMetricsOverride", {width: 1280, height: 900, deviceScaleFactor, mobile: false});
        await devTools.call("Emulation.setTouchEmulationEnabled", {enabled: deviceScaleFactor === 2, maxTouchPoints: 1});
        geometry.push(await devTools.evaluate(`(async()=>{
            const {pane,list}=windowingQualification;
            document.documentElement.style.fontSize='20px';
            pane.scrollTop=pane.scrollHeight;list.scrollTop=list.scrollHeight;
            for(let n=0;n<4;++n){await new Promise(done=>requestAnimationFrame(done));}
            return {agents:pane.querySelectorAll('.agent-card').length,threads:list.querySelectorAll('.thread-row').length,last:Number([...pane.querySelectorAll('[role=listitem]')].at(-1).getAttribute('aria-posinset')),overflow:document.documentElement.scrollWidth-innerWidth};
        })()`));
    }
    for (const check of geometry) {
        assert(check.agents < 100 && check.threads < 100);
        assert.equal(check.last, 10000);
        assert.equal(check.overflow, 0);
    }
    const grouping = await devTools.evaluate(`(async()=>{
        const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));
        const socket=codexuiApplicationProfile.socket, original=socket.send.bind(socket);
        const rows=Array.from({length:10000},(_,i)=>({id:'window-thread-'+i,name:'Window thread '+i,lineageKnown:true,parentThreadId:null,forkedFromId:null,projectId:'menu-project',section:i<2000?{id:'menu-section',name:'Existing section'}:null,status:{type:'idle'},turns:[]}));
        socket.send=data=>{const e=JSON.parse(data),r=e.payload;if(e.kind==='appserver'&&['thread/list','project/list','threadSection/list'].includes(r?.method)){socket.sent.push(e);const data=r.method==='project/list'?[{id:'menu-project',name:'Existing project',roots:[]}]:r.method==='threadSection/list'?[{id:'menu-section',name:'Existing section'}]:r.params?.sectionId?rows.filter(row=>row.section?.id===r.params.sectionId):rows;queueMicrotask(()=>socket.receive({kind:'appserver',payload:{jsonrpc:'2.0',id:r.id,result:{data,nextCursor:null}}}));}else{original(data);}};
        document.querySelector('.thread-browser-menu > button').click();await frame();
        const select=document.querySelector('[aria-label="Thread grouping"]');select.value='projects';select.dispatchEvent(new Event('change',{bubbles:true}));await frame();
        document.querySelector('.thread-browser-controls button').click();await frame();
        document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));await frame();
        const group=()=>[...document.querySelectorAll('.project-group')].find(group=>group.querySelector('header').textContent.includes('Existing project'));
        if(group().getAttribute('aria-expanded')!=='true'){group().querySelector('.group-disclosure').click();await frame();}
        const section=()=>group().querySelector('.nested-section');
        for(let n=0;n<100&&!section();++n){await new Promise(done=>setTimeout(done,5));}
        if(section().getAttribute('aria-expanded')!=='true'){section().querySelector('.group-disclosure').click();await frame();}
        const body=group().querySelector('.project-thread-body'),list=document.querySelector('.thread-list');
        body.scrollTop=0;list.scrollTop=0;await frame();
        const total=[...body.querySelectorAll('.scroll-window')].reduce((sum,node)=>sum+Number(node.dataset.windowCount),0);
        const nestedCount=Number(section().querySelector('.scroll-window').dataset.windowCount);
        const outer=list.scrollTop;body.scrollTop=body.scrollHeight;await frame();await frame();
        const final=body.querySelector('.scroll-window:last-child [data-window-key]:last-of-type [role=treeitem]')||[...body.querySelectorAll('[role=treeitem]')].at(-1);
        const answer={total,nestedCount,independent:list.scrollTop===outer,scrollable:body.scrollHeight>body.clientHeight,scrolled:body.scrollTop>100000,resident:body.querySelectorAll('.thread-row').length,lastPosition:Number(final?.getAttribute('aria-posinset')),lastSetSize:Number(final?.getAttribute('aria-setsize'))};
        group().querySelector('.group-disclosure').click();await frame();group().querySelector('.group-disclosure').click();await frame();
        answer.reopened=!!group().querySelector('.thread-row');socket.send=original;return answer;
    })()`);
    assert.equal(grouping.total, 10000);
    assert.equal(grouping.nestedCount, 2000);
    assert.equal(grouping.independent, true, "project body retains scroll ownership");
    assert.equal(grouping.scrollable, true);
    assert.equal(grouping.scrolled, true);
    assert.equal(grouping.lastPosition, grouping.lastSetSize);
    assert(grouping.resident < 100);
    assert.equal(grouping.reopened, true);
    return {...result, geometry, grouping};
}
