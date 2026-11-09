import assert from "node:assert/strict";
import {readFileSync} from "node:fs";

const fixture = JSON.parse(readFileSync(new URL("../../tests/fixtures/async-questions.json", import.meta.url)));

export async function qualifyAsyncQuestions(devTools) {
    const result = await devTools.evaluate(`(async()=>{
        const frame=()=>new Promise(done=>requestAnimationFrame(()=>requestAnimationFrame(done)));
        const waitFor=async(predicate)=>{for(let index=0;index<200&&!predicate();++index){await new Promise(done=>setTimeout(done,5));}
            if(!predicate())throw Error('Async question UI did not settle: '+document.querySelector('.thread-pane')?.textContent);};
        const socket=globalThis.codexuiApplicationProfile.socket,original=socket.send.bind(socket);
        const metadata={id:'async-question-ui',name:'Async question fixture',status:{type:'idle'},historyMode:'paginated'};
        socket.send=data=>{const envelope=JSON.parse(data),request=envelope.payload;
            if(envelope.kind==='appserver'&&request?.params?.threadId===metadata.id){
                socket.sent.push(envelope);
                if(request.method==='thread/resume'||request.method==='thread/turns/list'||request.method==='thread/items/list'){
                    queueMicrotask(()=>socket.receive({kind:'appserver',payload:{id:request.id,result:request.method==='thread/resume'
                        ?{thread:metadata}:{data:[],nextCursor:null}}}));}
            }else original(data);};
        const notify=(method,params)=>socket.receive({kind:'appserver',payload:{method,params}});
        [...document.querySelectorAll('.responsive-pane-button')].find(button=>button.textContent.includes('Threads'))?.click();await frame();
        notify('thread/started',{thread:metadata});
        await waitFor(()=>[...document.querySelectorAll('.thread-row')].some(row=>row.textContent.includes(metadata.name)));
        [...document.querySelectorAll('.thread-row')].find(row=>row.textContent.includes(metadata.name)).click();
        await frame();document.querySelector('[aria-label="Close Threads drawer"]')?.click();await frame();
        const prompt=document.querySelector('.composer textarea'),setter=Object.getOwnPropertyDescriptor(HTMLTextAreaElement.prototype,'value').set;
        setter.call(prompt,'Preserve my draft');prompt.dispatchEvent(new Event('input',{bubbles:true}));
        notify('turn/started',{threadId:metadata.id,turn:{id:'question-turn',status:'inProgress'}});
        const item={id:'question',type:'agentMessage',delivery:'async',phase:'commentary',text:'Approve the follow-up?',
            questions:[{title:'Approve the follow-up?',options:['Approve','Hold']}]};
        const updates=()=>document.querySelector('button[aria-label$="Codex update cards"]');
        const visibilityChecks={};
        const scroll=document.querySelector('.conversation-scroll');
        for(const entry of ${JSON.stringify(fixture.visibilityCases)}){
            const candidate={id:entry.id,type:'agentMessage',text:'Visibility '+entry.id,...entry.item};
            const visible=()=>[...document.querySelectorAll('.conversation-card.agentMessage')]
                .some(card=>card.textContent.includes(candidate.text));
            notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item:candidate});await frame();
            for(const [step,shown] of [false,true,false].entries()){
                if((updates().getAttribute('aria-pressed')==='true')!==shown){updates().click();await frame();}
                scroll.scrollTop=scroll.scrollHeight;await frame();
                visibilityChecks[entry.id+'-'+step+'-'+(shown?'updates-shown':'updates-hidden')]=
                    visible()===(shown||entry.visibleWhenUpdatesHidden);
            }
            notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item:{...candidate,phase:'commentary',questions:[]}});
            await frame();visibilityChecks[entry.id+'-cleared']=!visible();
        }
        notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item});
        await frame();scroll.scrollTop=scroll.scrollHeight;await frame();
        const answer=()=>[...document.querySelectorAll('.conversation-card button')].find(button=>button.textContent==='Answer…');
        await waitFor(()=>answer());
        const card=answer().closest('.conversation-card'),phase=card.querySelector('.card-phase');
        const rootStyle=getComputedStyle(document.documentElement);
        const hex=value=>'#'+value.match(/\\d+/g).map(channel=>Number(channel).toString(16).padStart(2,'0')).join('');
        const matches=(value,role)=>hex(value)===rootStyle.getPropertyValue(role).trim();
        const questionStyle=phase.textContent==='Question'
            &&matches(getComputedStyle(card).backgroundColor,'--question-message-surface')
            &&matches(getComputedStyle(card).borderTopColor,'--question-message-border')
            &&matches(getComputedStyle(phase).color,'--question-message-text')
            &&matches(getComputedStyle(card.querySelector('.card-heading > span')).color,'--question-message-text');
        answer().focus();answer().click();
        const dialog=()=>document.querySelector('dialog[aria-label="Codex needs input"]');
        await waitFor(()=>dialog()?.open);
        const modal=dialog().matches(':modal'),defaultChoice=dialog().querySelector('input[type="radio"]')?.checked===true;
        const cancel=()=>[...dialog().querySelectorAll('button')].find(button=>button.textContent==='Cancel').click();
        const submits=()=>socket.sent.filter(envelope=>['turn/steer','turn/start'].includes(envelope.payload?.method));
        const before=submits().length;cancel();await frame();
        const cancelled=submits().length===before,focusReturned=document.activeElement===answer();
        answer().focus();answer().click();await waitFor(()=>dialog()?.open);
        [...dialog().querySelectorAll('input[type="radio"]')].find(input=>input.closest('label').textContent.includes('Hold')).click();await frame();
        const exclusive=[...dialog().querySelectorAll('input[type="radio"]')].filter(input=>input.checked).length===1;
        [...dialog().querySelectorAll('button')].find(button=>button.textContent==='Submit').click();
        await waitFor(()=>!dialog());await frame();
        const sent=submits().at(-1)?.payload;
        const chosen=sent?.method==='turn/steer'&&sent.params.expectedTurnId==='question-turn'
            &&sent.params.threadId===metadata.id&&JSON.parse(sent.params.input[0].text.slice(34,-35))[0].answer==='Hold';
        const draftRetained=prompt.value==='Preserve my draft';
        answer().focus();answer().click();await waitFor(()=>dialog()?.open);
        const other=dialog().querySelector('input[placeholder="Other answer"]');
        Object.getOwnPropertyDescriptor(HTMLInputElement.prototype,'value').set.call(other,'My own answer');other.dispatchEvent(new Event('input',{bubbles:true}));await frame();
        const freeTextClearsChoice=[...dialog().querySelectorAll('input[type="radio"]')].every(input=>!input.checked);
        notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item:{...item,questions:[{title:'Changed question'}]}});await frame();
        const beforeStale=submits().length;
        [...dialog().querySelectorAll('button')].find(button=>button.textContent==='Submit').click();await frame();
        const staleRetained=dialog()?.open&&other.value==='My own answer'&&submits().length===beforeStale&&!!dialog().querySelector('[role="alert"]');
        cancel();await frame();
        notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item:{...item,phase:'final_answer',questions:[]}});await frame();
        const finalRestored=card.querySelector('.card-phase')?.textContent==='final answer'&&!answer()
            &&matches(getComputedStyle(card).backgroundColor,'--final-answer-surface');
        socket.receive({kind:'appserver',payload:{id:sent.id,result:{turnId:'question-turn'}}});await frame();
        const entry=${JSON.stringify(fixture.cases.find(entry=>entry.id==='two-independent-choices'))};
        const two={id:'two-question',type:'agentMessage',delivery:'async',phase:'commentary',
            text:entry.questions.map(q=>q.title+'\\n'+q.options.map(option=>'- '+option).join('\\n')).join('\\n\\n'),questions:entry.questions};
        notify('item/completed',{threadId:metadata.id,turnId:'question-turn',item:two});await frame();
        scroll.scrollTop=scroll.scrollHeight;await frame();
        const twoCard=()=>[...document.querySelectorAll('.conversation-card.agentMessage')].find(card=>card.textContent.includes('Merge the changes?'));
        const twoAnswer=()=>[...twoCard().querySelectorAll('button')].find(button=>button.textContent==='Answer…'||button.textContent==='Answer again…');
        twoAnswer().click();await waitFor(()=>dialog()?.open);
        const fields=[...dialog().querySelectorAll('fieldset')];
        fields.forEach((field,index)=>[...field.querySelectorAll('input[type="radio"]')]
            .find(input=>input.closest('label').textContent.trim()===entry.answer[String(index)].answers[0]).click());await frame();
        const independentChoices=fields.every((field,index)=>[...field.querySelectorAll('input[type="radio"]')]
            .filter(input=>input.checked).map(input=>input.closest('label').textContent.trim()).join()===entry.answer[String(index)].answers[0]);
        [...dialog().querySelectorAll('button')].find(button=>button.textContent==='Submit').click();await waitFor(()=>!dialog());await frame();
        const reply=submits().at(-1).payload;
        const submitted=JSON.parse(reply.params.input[0].text.slice(34,-35));
        const twoQuestionsPreserved=submitted.length===2&&submitted.every((answer,index)=>answer.question===entry.questions[index].title
            &&answer.answer===entry.answer[String(index)].answers[0]&&JSON.parse(answer.questionItemId)[1]===two.id);
        const sendingHeader=twoCard().querySelector('.card-phase').textContent==='Question · Sending…';
        socket.receive({kind:'appserver',payload:{id:reply.id,result:{turnId:'question-turn'}}});await frame();
        const acknowledgedHeader=twoCard().querySelector('.card-phase').textContent==='Question · ✓ Answer sent';
        const boldAnswers=[...twoCard().querySelectorAll('strong')].map(element=>element.textContent).join('|')==='Hold|Production';
        const answerAgain=twoAnswer().textContent==='Answer again…';
        const oldStyle=twoCard().getAttribute('style');
        const narrowStatus=[300,440,900].every(width=>{
            twoCard().style.width=width+'px';twoCard().style.boxSizing='border-box';
            const phase=twoCard().querySelector('.card-phase'),bounds=phase.getBoundingClientRect();
            const range=document.createRange();range.selectNodeContents(phase);
            return [...range.getClientRects()].every(rect=>rect.left>=bounds.left-.5&&rect.right<=bounds.right+.5&&rect.bottom<=bounds.bottom+.5);
        });
        if(oldStyle===null){twoCard().removeAttribute('style');}else{twoCard().setAttribute('style',oldStyle);}
        socket.send=original;
        return {...visibilityChecks,questionStyle,finalRestored,modal,defaultChoice,cancelled,focusReturned,exclusive,chosen,draftRetained,freeTextClearsChoice,staleRetained,independentChoices,twoQuestionsPreserved,sendingHeader,acknowledgedHeader,boldAnswers,answerAgain,narrowStatus};
    })()`);
    console.log(JSON.stringify({asyncQuestionChecks: result}));
    for (const [name, passed] of Object.entries(result)) {
        assert.equal(passed, true, `Async question: ${name}`);
    }
    return result;
}
