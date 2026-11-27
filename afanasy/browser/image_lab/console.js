"use strict";
const el = id => document.getElementById(id);
const create = (tag, text) => Object.assign(document.createElement(tag), {textContent:text || ""});
let user, key, cursor = 0, events = [], selected = 0, job, busy = false, version = 0, listSignature;
const labels = {dispatched:"派发", started:"开始", killed:"已终止", failed:"失败", requeued:"重新入队",
                committed:"提交完成", commit_failed:"提交失败"};
const fmt = ms => new Date(ms).toLocaleTimeString("zh-CN", {hour12:false, fractionalSecondDigits:3,hour:"2-digit",minute:"2-digit",second:"2-digit"});
function message(error) { el("notice").textContent = error.message || String(error); }
async function api(path, options={}) {
    const r = await fetch("/api/"+path, options);
    if (!r.ok) throw new Error(({400:"输入或图片格式无效",401:"请登录或检查账号密码",403:"权限不足",404:"作业或图块不可用",409:"当前尝试已结束或结果尚未完成",413:"图片超过上传大小限制"})[r.status] || "服务暂时不可用");
    return r.json();
}
function post(path, data) {
    const form = data instanceof FormData;
    return api(path,{method:"POST",headers:form?{"X-Image-Lab":"1"}:{"X-Image-Lab":"1","Content-Type":"application/json"},
                     body:form?data:JSON.stringify(data)});
}
function signedIn(info) {
    user=info; el("login").hidden=true; el("console").hidden=false; el("logout").hidden=false;
    el("identity").textContent=info.name+" · "+(info.role==="admin"?"管理员":"普通用户");
    el("jobsTitle").textContent=info.role==="admin"?"全部作业":"我的作业"; refresh();
}
el("login").onsubmit=async e=>{
    e.preventDefault();
    try {signedIn(await post("login",Object.fromEntries(new FormData(e.target))));e.target.reset();el("notice").textContent="";}
    catch(err){message(err);}
};
el("logout").onclick=async()=>{try{await post("logout",{});location.reload();}catch(e){message(e);}};
el("upload").onsubmit=async e=>{
    e.preventDefault(); const button=e.target.querySelector("button"); button.disabled=true;
    try {const result=await post("jobs",new FormData(e.target));e.target.reset();choose(result.key);el("notice").textContent="";}
    catch(err){message(err);} finally{button.disabled=false;}
};
function choose(id) {
    key=id;version++;cursor=0;events=[];selected=0;job=null;listSignature=null;
    el("workspace").hidden=true;el("canvas").replaceChildren();refresh();
}
async function refresh() {
    if (!user || busy) return;
    busy=true;const at=version;
    try {
        const jobs=await api("jobs");
        if(at!==version)return;
        const signature=JSON.stringify(jobs);
        if(signature!==listSignature){
            el("jobs").replaceChildren(...jobs.map(j=>{
                const b=create("button",j.filename+" · "+j.state);
                b.setAttribute("aria-current",String(j.key===key));b.onclick=()=>choose(j.key);return b;
            }));listSignature=signature;
        }
        if(!key)return;
        const path="jobs/"+key;
        const [item,delta]=await Promise.all([api(path),api(path+"/events?cursor="+cursor)]);
        if(at!==version)return;
        const changed=!job || delta.events.length || job.committed.length!==item.committed.length || job.complete!==item.complete;
        job=item;cursor=delta.cursor;events.push(...delta.events);
        el("workspace").hidden=false;el("empty").hidden=true;
        if(changed)render();
    } catch(e){if(at===version)message(e);} finally{busy=false;}
}
function render(){
    const {width,height,side,committed}=job, cols=Math.ceil(width/side), rows=Math.ceil(height/side);
    const total=rows*cols, done=new Set(committed), attempts=new Map(), tileEvents=new Map();
    for(const e of events){
        if(!tileEvents.has(e.tile))tileEvents.set(e.tile,[]);
        tileEvents.get(e.tile).push(e);
        const id=e.tile+"/"+e.attempt;
        if(e.state==="started")attempts.set(id,{start:e});
        if((e.state.startsWith("exit-")||e.state.startsWith("signal-")) && attempts.has(id))attempts.get(id).end=e;
    }
    el("title").textContent=job.filename;el("summary").textContent=width+" × "+height+" · "+(job.operation==="gray"?"灰度":"反色")+" · "+(job.priority==="high"?"高":"普通")+"优先级";
    el("progress").textContent=done.size+" / "+total+" 块已提交";
    el("download").hidden=!job.complete;el("download").href="/api/jobs/"+key+"/download";
    const grid=el("canvas");grid.style.aspectRatio=width+"/"+height;
    grid.style.gridTemplateColumns="repeat("+cols+", minmax(0, 1fr))";
    grid.style.gridTemplateRows="repeat("+rows+", minmax(0, 1fr))";
    if(cols>1)grid.style.gridTemplateColumns="repeat("+(cols-1)+",minmax(0,1fr)) minmax(0,"+((width-(cols-1)*side)/side)+"fr)";
    if(rows>1)grid.style.gridTemplateRows="repeat("+(rows-1)+",minmax(0,1fr)) minmax(0,"+((height-(rows-1)*side)/side)+"fr)";
    for(let t=0;t<total;t++){
        let b=grid.children[t];
        if(!b){b=create("button");b.className="tile";b.append(create("span",String(t)));b.setAttribute("aria-label","图块 "+t);b.onclick=()=>{selected=t;render();};grid.append(b);}
        const history=tileEvents.get(t)||[], last=history[history.length-1];
        const state=done.has(t)?"已完成":last?.state==="started"?"运行中":"等待";
        b.title="图块 "+t+" · "+state;b.classList.toggle("running",state==="运行中");
        b.classList.toggle("retry",history.some(e=>e.attempt>1||e.state==="requeued"));
        b.classList.toggle("selected",t===selected);
        if(done.has(t)&&!b.querySelector("img")){const img=create("img");img.alt="已处理图块 "+t;img.src="/api/jobs/"+key+"/tiles/"+t;b.append(img);}
    }
    const runs=[...attempts.values()], now=Date.now();
    const first=runs.length?runs[0].start.time_ms:now;
    const lastTime=runs.reduce((max,r)=>Math.max(max,r.end?.time_ms??now),first+1);
    el("timeline").replaceChildren(...runs.map(({start,end})=>{
        const lane=create("div");lane.className="lane";lane.dataset.pid=start.pid;
        const pick=create("button","PID "+start.pid+" · 图块 "+start.tile+" · 第 "+start.attempt+" 次");
        pick.onclick=()=>{selected=start.tile;render();};lane.append(pick);
        const track=create("div");track.className="track";const bar=create("span");bar.className="bar-segment";
        bar.style.left=(start.time_ms-first)*100/(lastTime-first)+"%";
        bar.style.width=Math.max(0,((end?.time_ms??now)-start.time_ms)*100/(lastTime-first))+"%";
        track.append(bar);lane.append(track,create("small",fmt(start.time_ms)+" – "+(end?fmt(end.time_ms):"运行中")));
        if(!end&&user.role==="admin"){const stop=create("button","终止 PID "+start.pid);
            stop.onclick=async()=>{stop.disabled=true;selected=start.tile;try{await post("jobs/"+key+"/terminate",start);await refresh();}catch(e){message(e);}};lane.append(stop);}
        return lane;
    }));
    const history=tileEvents.get(selected)||[], attemptsForTile=runs.filter(r=>r.start.tile===selected);
    el("historyTitle").textContent="图块 "+selected+" · 执行历史";
    // Reuse the original Afanasy monitor task row and its state renderer.
    const native=new TaskItem(null,null,selected);native.element=create("div");native.monitor={document};native.init();
    native.elName.textContent="图块 "+selected;
    native.updateProgress({state:done.has(selected)?"DON":attemptsForTile.some(r=>!r.end)?"RUN":"RDY",
                           str:attemptsForTile.length,err:history.filter(e=>e.state==="killed"||e.state==="failed").length});
    el("nativeTask").replaceChildren(native.element);
    el("history").replaceChildren(...history.map(e=>create("li",fmt(e.time_ms)+" · 第 "+e.attempt+" 次 · "+(labels[e.state]||e.state)+(e.pid?" · PID "+e.pid:""))));
}
api("me").then(signedIn).catch(()=>{});setInterval(refresh,800);
