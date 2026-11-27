// Real Chromium contexts with independent cookie jars. Two contexts, three accounts.
const {chromium}=require("playwright");
const assert=require("node:assert/strict");
const fs=require("node:fs/promises");
const path=require("node:path");
const [base,root]=process.argv.slice(2);
const password=process.env.IMAGE_TEST_PASSWORD;

async function run(){
    const browser=await chromium.launch({executablePath:process.env.CHROME_PATH||"/usr/bin/google-chrome",headless:true});
    const errors=[];
    try {
        const ordinary=await browser.newContext({acceptDownloads:true,locale:"zh-CN"});
        const operator=await browser.newContext({acceptDownloads:true,locale:"zh-CN"});
        const a=await ordinary.newPage(), b=await operator.newPage();
        for(const page of [a,b])page.on("pageerror",err=>errors.push(err.message));
        async function login(page,user){
            await page.goto(base);
            await page.getByLabel("用户名").fill(user);await page.getByLabel("密码").fill(password);
            await page.getByRole("button",{name:"登录",exact:true}).click();
            await page.getByRole("heading",{name:user==="manager"?"全部作业":"我的作业"}).waitFor();
        }
        async function submit(page,file){
            await page.getByLabel("图片",{exact:true}).setInputFiles(path.join(root,file));
            const ack=page.waitForResponse(r=>r.url().endsWith("/api/jobs")&&r.request().method()==="POST");
            await page.getByRole("button",{name:"提交作业",exact:true}).click();
            const response=await ack;assert.equal(response.status(),201);
            return (await response.json()).key;
        }
        await login(a,"alice");await login(b,"bob");
        const foreign=await submit(b,"source.jpg");
        for(const suffix of ["","/events","/tiles/0","/download"]){
            assert.equal((await ordinary.request.get(base+"/api/jobs/"+foreign+suffix)).status(),404);
        }
        assert(!(await (await ordinary.request.get(base+"/api/jobs")).json()).some(j=>j.key===foreign));
        assert.equal((await ordinary.request.get(base+"/api/queue")).status(),403);
        assert.equal(await a.locator("#jobs button").filter({hasText:"source.jpg"}).count(),0);
        assert.equal((await ordinary.request.post(base+"/api/jobs/"+foreign+"/terminate",
            {headers:{"X-Image-Lab":"1"},data:{tile:0,attempt:1,pid:1}})).status(),403);
        await b.getByRole("button",{name:"退出登录"}).click();
        await b.getByRole("button",{name:"登录",exact:true}).waitFor();
        assert.equal((await operator.request.get(base+"/api/jobs")).status(),401);
        await login(b,"manager");
        const key=await submit(a,"source.png");
        await b.getByRole("button",{name:/source\.png ·/}).click({timeout:30000});
        const stop=b.getByRole("button",{name:/^终止 PID /}).first();
        await stop.waitFor({timeout:60000});
        const old=Number((await stop.textContent()).split(" ").pop());
        const lane=b.locator('.lane[data-pid="'+old+'"]');
        const tile=Number((await lane.locator("button").first().textContent()).match(/图块 (\d+)/)[1]);
        // Target this specific PID even if the next refresh reorders rows.
        await b.getByRole("button",{name:"终止 PID "+old,exact:true}).click();
        await b.getByRole("button",{name:"图块 "+tile,exact:true}).click();
        const history=b.locator("#history");
        await history.getByText(/重新入队/).waitFor({timeout:60000});
        await history.getByText(/第 2 次 · 开始 · PID/).waitFor({timeout:60000});
        const next=Number((await history.getByText(/第 2 次 · 开始 · PID/).textContent()).match(/PID (\d+)/)[1]);
        assert.notEqual(old,next);
        assert(await history.getByText(new RegExp("第 1 次 · 开始 · PID "+old+"$")).count());
        await history.getByText(/第 2 次 · 提交完成/).waitFor({timeout:90000});
        await b.screenshot({path:path.join(root,"admin-recovery.png"),fullPage:true});
        await a.getByRole("link",{name:"下载 PNG"}).waitFor({timeout:90000});
        await a.waitForFunction(()=>Array.from(document.querySelectorAll("#canvas img")).length===9&&
            Array.from(document.querySelectorAll("#canvas img")).every(img=>img.complete&&img.naturalWidth>0));
        const last=a.getByRole("button",{name:"图块 8",exact:true}).locator("img");
        assert.deepEqual(await last.evaluate(img=>[img.naturalWidth,img.naturalHeight]),[2,22]);
        await a.screenshot({path:path.join(root,"user-completed.png"),fullPage:true});
        const pending=a.waitForEvent("download");await a.getByRole("link",{name:"下载 PNG"}).click();
        const download=await pending;
        assert.deepEqual(await fs.readFile(await download.path()),await fs.readFile(path.join(root,"png-gray.png")));
        assert(!await a.getByRole("button",{name:/终止 PID/}).count());
        assert.deepEqual(errors,[]);
        console.log("PASS two browser sessions, owner isolation, admin kill: tile",tile,"PID",old,"->",next,
                    "attempt 2 committed; 9 real previews, correct PNG download; job",key);
    }finally{await browser.close();}
}
run().catch(e=>{console.error(e);process.exitCode=1;});
