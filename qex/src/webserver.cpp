#include "webserver.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <algorithm>
#include <sstream>

namespace qex {

namespace {

// ------------------------------------------------ tiny JSON value parser
struct JsonVal {
    std::string str;
    std::unordered_map<std::string, std::string> fields;
    std::string get(const std::string &k, const std::string &d = "") const {
        auto it = fields.find(k);
        return it == fields.end() ? d : it->second;
    }
};

JsonVal parse_json_object(const std::string &body) {
    JsonVal v;
    size_t i = 0;
    auto skipWs = [&] {
        while (i < body.size() &&
               (body[i] == ' ' || body[i] == '\t' || body[i] == '\n' ||
                body[i] == '\r'))
            ++i;
    };
    skipWs();
    if (i < body.size() && body[i] == '{') ++i;
    while (i < body.size()) {
        skipWs();
        if (i < body.size() && body[i] == '}') break;
        if (body[i] != '"') { ++i; continue; }
        ++i;
        std::string key;
        while (i < body.size() && body[i] != '"') key.push_back(body[i++]);
        if (i < body.size()) ++i;               // closing quote
        skipWs();
        if (i < body.size() && body[i] == ':') ++i;
        skipWs();
        std::string val;
        if (i < body.size() && body[i] == '"') {
            ++i;
            while (i < body.size() && body[i] != '"') {
                if (body[i] == '\\' && i + 1 < body.size()) {
                    char e = body[++i];
                    val.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
                    ++i;
                } else {
                    val.push_back(body[i++]);
                }
            }
            if (i < body.size()) ++i;           // closing quote
        } else {
            while (i < body.size() && body[i] != ',' && body[i] != '}')
                val.push_back(body[i++]);
            val = trim(val);
        }
        v.fields[key] = val;
        skipWs();
        if (i < body.size() && body[i] == ',') ++i;
    }
    return v;
}

// --------------------------------------------------------- JSON builders
std::string jstr(const std::string &s) { return "\"" + json_escape(s) + "\""; }

std::string resultJson(const QueryResult &r) {
    std::ostringstream o;
    o << "{";
    o << "\"index\":" << r.queryIndex;
    o << ",\"sql\":" << jstr(r.sql);
    o << ",\"structHash\":" << jstr(r.structHashHex);
    o << ",\"rows\":" << r.rows;
    o << ",\"selected\":" << r.selected;
    o << ",\"match\":" << (r.match ? "true" : "false");
    o << ",\"firstDiff\":" << r.firstDiff;
    o << ",\"fallback\":" << (r.usedFallback ? "true" : "false");
    o << ",\"fallbackReason\":" << jstr(r.fallbackReason);
    o << ",\"source\":" << jstr(jit_source_name(r.compile.source));
    o << ",\"codegenMs\":" << r.codegenMs;
    o << ",\"compileMs\":" << r.compile.compileMs;
    o << ",\"interpFilterMs\":" << r.interpFilterMs;
    o << ",\"interpProjectMs\":" << r.interpProjectMs;
    o << ",\"interpTotalMs\":" << r.interpTotalMs;
    o << ",\"jitFilterMs\":" << r.jitFilterMs;
    o << ",\"jitProjectMs\":" << r.jitProjectMs;
    o << ",\"jitTotalMs\":" << r.jitTotalMs;
    o << ",\"speedup\":" << r.speedup;
    o << ",\"growsPerSec\":" << r.jitMetrics.growsPerSec;
    o << ",\"vecFilter\":" << (r.compile.vecFilter ? "true" : "false");
    o << ",\"vecLoopCount\":" << r.compile.vecLoopCount;
    o << ",\"cseCount\":" << r.opt.cseCount;
    o << ",\"andReorders\":" << r.opt.andReorders;
    o << ",\"soPath\":" << jstr(r.compile.soPath);
    o << ",\"cPath\":" << jstr(r.compile.cPath);
    o << ",\"spreds\":[";
    for (size_t i = 0; i < r.spredKinds.size(); ++i) {
        if (i) o << ",";
        o << jstr(r.spredKinds[i]);
    }
    o << "],\"cseList\":[";
    for (size_t i = 0; i < r.opt.cseList.size(); ++i) {
        if (i) o << ",";
        o << jstr(r.opt.cseList[i]);
    }
    o << "],\"orderList\":[";
    for (size_t i = 0; i < r.opt.orderList.size(); ++i) {
        if (i) o << ",";
        o << jstr(r.opt.orderList[i]);
    }
    o << "],\"vecRemarks\":[";
    for (size_t i = 0; i < r.compile.vecRemarks.size(); ++i) {
        if (i) o << ",";
        o << jstr(r.compile.vecRemarks[i]);
    }
    o << "],\"generatedC\":" << jstr(r.generatedC);
    o << "}";
    return o.str();
}

const char *DASHBOARD_HTML = R"HTML(
<!doctype html><html lang="zh"><head><meta charset="utf-8">
<title>qex · 查询执行器与运行时代码生成</title>
<style>
:root{--bd:#263041;--bg:#0f1622;--card:#17202f;--mut:#8aa0bd;--acc:#5db1ff;
--ok:#46d39a;--warn:#ffce6b;--bad:#ff7a7a;}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:#dce7f5;
font:14px/1.6 -apple-system,Segoe UI,Roboto,"PingFang SC",sans-serif}
header{padding:16px 24px;border-bottom:1px solid var(--bd);display:flex;
gap:20px;align-items:center;flex-wrap:wrap}
h1{font-size:18px;margin:0} .tag{color:var(--mut);font-size:12px}
.tabs{display:flex;gap:4px;padding:0 24px;border-bottom:1px solid var(--bd);
background:#111a28}
.tabs button{background:none;border:0;color:var(--mut);padding:12px 18px;
cursor:pointer;font-size:14px;border-bottom:2px solid transparent}
.tabs button.on{color:#fff;border-bottom-color:var(--acc)}
main{padding:20px 24px;display:grid;gap:16px}
.card{background:var(--card);border:1px solid var(--bd);border-radius:10px;
padding:16px}
.card h2{margin:0 0 10px;font-size:15px}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}
.kpi{background:#111a28;border:1px solid var(--bd);border-radius:8px;padding:12px}
.kpi b{display:block;font-size:22px;margin-top:4px}
.kpi span{color:var(--mut);font-size:12px}
table{width:100%;border-collapse:collapse;font-size:13px}
th,td{text-align:left;padding:8px 10px;border-bottom:1px solid var(--bd);
vertical-align:top}
th{color:var(--mut);font-weight:600}
textarea,input,select{width:100%;background:#0c1320;color:#dce7f5;
border:1px solid var(--bd);border-radius:8px;padding:10px;font:inherit}
textarea{min-height:140px;font-family:ui-monospace,Menlo,monospace;font-size:13px}
button.act{background:var(--acc);color:#061321;border:0;border-radius:8px;
padding:10px 18px;font-weight:700;cursor:pointer}
button.ghost{background:transparent;color:var(--acc);border:1px solid var(--acc);
border-radius:8px;padding:8px 14px;cursor:pointer}
pre{background:#0a101b;border:1px solid var(--bd);border-radius:8px;padding:12px;
overflow:auto;max-height:520px;font:12px/1.55 ui-monospace,Menlo,monospace;
white-space:pre}
.pill{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px}
.pill.ok{background:#103326;color:var(--ok)}
.pill.warn{background:#3a2f12;color:var(--warn)}
.pill.bad{background:#3b1a1c;color:var(--bad)}
.pill.info{background:#132c47;color:var(--acc)}
.mut{color:var(--mut)} .row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.bar{height:8px;background:#0c1320;border-radius:4px;overflow:hidden}
.bar i{display:block;height:100%;background:var(--acc)}
code.k{color:#9ad0ff}
</style></head><body>
<header><h1>qex 查询执行器</h1>
<span class="tag">运行时代码生成 · 本机编译器 · 位图化空值 · 向量化执行</span>
<span id="env" class="tag"></span></header>
<nav class="tabs">
<button data-t="submit" class="on">① 查询提交</button>
<button data-t="code">② 生成代码</button>
<button data-t="cache">③ 编译缓存</button>
<button data-t="vec">④ 向量化情况</button>
<button data-t="perf">⑤ 性能对比</button>
</nav><main>

<section id="submit">
 <div class="card"><h2>提交查询</h2>
 <textarea id="sql">SELECT service, latency_us, latency_us / 1000 AS ms
FROM logs WHERE level = 'ERROR' AND latency_us > 5000 AND status >= 500</textarea>
 <div class="row" style="margin-top:10px">
 <button class="act" onclick="submitQ(false)">执行查询</button>
 <button class="ghost" onclick="submitQ(true)">执行并逐行校验</button>
 <label class="mut"><input type="checkbox" id="verify" style="width:auto" checked> 与解释执行比对</label>
 </div></div>
 <div class="card" id="lastCard" style="display:none"><h2>最近一次执行</h2>
 <div id="last"></div></div>
 <div class="card"><h2>历史查询</h2><table id="hist"><thead><tr>
 <th>#</th><th>SQL</th><th>来源</th><th>命中行</th><th>校验</th>
 <th>解释(ms)</th><th>JIT(ms)</th><th>加速比</th><th></th></tr></thead>
 <tbody></tbody></table></div>
</section>

<section id="code" style="display:none">
 <div class="card"><h2>生成的 C 代码</h2>
 <div class="row mut" style="margin-bottom:8px">
 <span>结构哈希:</span><code id="chash" class="k">-</code>
 <span id="cmode" class="pill info"></span></div>
 <pre id="ccode">执行一条查询后这里展示生成的 C 翻译单元</pre></div>
 <div class="card"><h2>优化决策</h2><pre id="opt">-</pre></div>
</section>

<section id="cache" style="display:none">
 <div class="grid" id="cacheKpis"></div>
 <div class="card" style="margin-top:16px"><h2>缓存动作与编译耗时</h2>
 <table id="cacheTab"><thead><tr><th>#</th><th>结构哈希</th><th>来源</th>
 <th>编译耗时(ms)</th><th>代码生成(ms)</th><th>回退原因</th></tr></thead>
 <tbody></tbody></table></div>
 <div class="card"><h2>磁盘缓存</h2><div id="diskInfo" class="mut"></div>
 <button class="ghost" style="margin-top:10px" onclick="clearCache()">清空磁盘缓存</button></div>
</section>

<section id="vec" style="display:none">
 <div class="card"><h2>向量化与字符串专用路径</h2>
 <table id="vecTab"><thead><tr><th>#</th><th>SQL</th>
 <th>向量化循环</th><th>字符串谓词专用路径</th><th>编译器备注</th></tr></thead>
 <tbody></tbody></table></div>
 <div class="card"><h2>编译器向量化备注（最近查询）</h2><pre id="vecRemark">-</pre></div>
</section>

<section id="perf" style="display:none">
 <div class="grid" id="perfKpis"></div>
 <div class="card" style="margin-top:16px"><h2>逐查询性能对比</h2>
 <table id="perfTab"><thead><tr><th>#</th><th>SQL</th>
 <th>解释总耗时</th><th>JIT总耗时</th><th>加速比</th>
 <th>吞吐(G行/s)</th><th>编译占比</th></tr></thead><tbody></tbody></table></div>
 <div class="card"><h2>加速比</h2><div id="bars"></div></div>
</section>
</main>
<script>
let LAST=null;
const $=id=>document.getElementById(id);
document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{
 document.querySelectorAll('.tabs button').forEach(x=>x.classList.remove('on'));
 b.classList.add('on');
 document.querySelectorAll('main section').forEach(s=>s.style.display='none');
 $(b.dataset.t).style.display=''; refresh(b.dataset.t);
});
async function api(path,opts){const r=await fetch(path,opts);return r.json();}
async function submitQ(forceVerify){
 const body=JSON.stringify({sql:$('sql').value,
  verify: forceVerify || $('verify').checked});
 const r=await api('/api/query',{method:'POST',
  headers:{'Content-Type':'application/json'},body});
 if(r.error){alert(r.error);return;}
 LAST=r.result; showLast(r.result); refreshAll();
}
function pill(cls,txt){return '<span class="pill '+cls+'">'+txt+'</span>'}
function showLast(r){$('lastCard').style.display='';
 $('last').innerHTML=`<div class="grid">
 <div class="kpi"><span>执行来源</span><b>${r.source}</b></div>
 <div class="kpi"><span>命中行数</span><b>${r.selected.toLocaleString()}</b></div>
 <div class="kpi"><span>解释 / JIT (ms)</span><b>${r.interpTotalMs.toFixed(2)} / ${r.jitTotalMs.toFixed(2)}</b></div>
 <div class="kpi"><span>加速比</span><b>${r.speedup.toFixed(2)}×</b></div>
 <div class="kpi"><span>逐行校验</span><b>${r.match?pill('ok','一致'):pill('bad','不一致 @'+r.firstDiff)}</b></div>
 <div class="kpi"><span>编译耗时</span><b>${r.compileMs.toFixed(1)} ms</b></div></div>
 ${r.fallback?'<p style="margin-top:10px">'+pill('warn','已回退解释执行')+' <span class="mut">'+r.fallbackReason+'</span></p>':''}`;
 $('chash').textContent=r.structHash;
 $('cmode').textContent=r.source;
 $('ccode').textContent=r.generatedC;
 $('opt').textContent='公共子表达式合并: '+r.cseCount+' 个\n'+
  r.cseList.map(x=>'  CSE: '+x).join('\n')+
  '\nAND 选择率重排: '+r.andReorders+' 处\n'+
  r.orderList.map(x=>'  '+x).join('\n')+
  '\n字符串专用路径:\n'+r.spreds.map(x=>'  '+x).join('\n');
 $('vecRemark').textContent=r.vecRemarks.join('\n')||'(无向量化备注)';
}
async function refreshAll(){await refresh('submit');await refresh('cache');
 await refresh('vec');await refresh('perf');}
async function refresh(t){
 const s=await api('/api/state');
 $('env').textContent=s.table.nrows.toLocaleString()+' 行 · '+
  s.table.byteText+' · 编译器: '+s.jit.compiler.split('|')[0];
 const H=s.history;
 if(t==='submit'){$('hist').querySelector('tbody').innerHTML=H.map(r=>
  `<tr><td>${r.index}</td><td style="max-width:340px">${r.sql}</td>
  <td>${pill(r.source.includes('cache')?'info':r.fallback?'warn':'ok',r.source)}</td>
  <td>${r.selected.toLocaleString()}</td>
  <td>${r.match?pill('ok','一致'):pill('bad','差异')}</td>
  <td>${r.interpTotalMs.toFixed(2)}</td><td>${r.jitTotalMs.toFixed(2)}</td>
  <td><b>${r.speedup.toFixed(2)}×</b></td>
  <td><button class="ghost" onclick='loadHist(${r.index})'>查看</button></td></tr>`).join('');}
 if(t==='cache'){const j=s.jit;
  $('cacheKpis').innerHTML=[['查询总数',j.lookups],['内存缓存命中',j.memHits],
   ['磁盘缓存命中',j.diskHits],['实时编译',j.compiles],['回退次数',j.fallbacks],
   ['平均编译ms',j.avgCompileMs.toFixed(1)],['磁盘缓存省下(ms)',j.savedDisk.toFixed(1)],
   ['内存缓存省下(ms)',j.savedMem.toFixed(1)]].map(k=>
   `<div class="kpi"><span>${k[0]}</span><b>${k[1]}</b></div>`).join('');
  $('cacheTab').querySelector('tbody').innerHTML=H.map(r=>
   `<tr><td>${r.index}</td><td><code>${r.structHash.slice(0,16)}</code></td>
   <td>${r.source}</td><td>${r.compileMs.toFixed(1)}</td>
   <td>${r.codegenMs.toFixed(2)}</td>
   <td class="mut">${r.fallbackReason||''}</td></tr>`).join('');
  $('diskInfo').textContent=s.disk;}
 if(t==='vec'){$('vecTab').querySelector('tbody').innerHTML=H.map(r=>
  `<tr><td>${r.index}</td><td style="max-width:300px">${r.sql}</td>
  <td>${r.vecFilter?pill('ok',r.vecLoopCount+' 个循环向量化'):pill('warn','未向量化')}</td>
  <td>${r.spreds.length?r.spreds.map(x=>pill('info',x.split('-> ')[1]||x)).join(' '):'<span class="mut">-</span>'}</td>
  <td class="mut" style="max-width:280px">${(r.vecRemarks[0]||'').slice(0,120)}</td></tr>`).join('');}
 if(t==='perf'){const sm=s.summary;
  $('perfKpis').innerHTML=[['查询数',sm.totalQueries],
   ['几何平均加速比',sm.geoMeanSpeedup.toFixed(2)+'×'],
   ['解释总耗时(s)',(sm.totalInterpMs/1000).toFixed(3)],
   ['JIT总耗时(s)',(sm.totalJitMs/1000).toFixed(3)],
   ['编译总耗时(s)',(sm.totalCompileMs/1000).toFixed(3)],
   ['编译耗时占比',sm.compileSharePct.toFixed(1)+'%'],
   ['结果一致',sm.matched+' / '+sm.totalQueries]].map(k=>
   `<div class="kpi"><span>${k[0]}</span><b>${k[1]}</b></div>`).join('');
  $('perfTab').querySelector('tbody').innerHTML=H.map(r=>
   `<tr><td>${r.index}</td><td style="max-width:300px">${r.sql}</td>
   <td>${r.interpTotalMs.toFixed(2)}</td><td>${r.jitTotalMs.toFixed(2)}</td>
   <td><b>${r.speedup.toFixed(2)}×</b></td><td>${r.growsPerSec.toFixed(3)}</td>
   <td>${r.source==='fresh-compile'?(100*r.compileMs/(r.compileMs+r.jitTotalMs)).toFixed(1)+'%':'-'}</td></tr>`).join('');
  const mx=Math.max(1,...H.map(r=>r.speedup));
  $('bars').innerHTML=H.map(r=>
   `<div style="margin:6px 0"><div class="row mut"><span style="width:40px">#${r.index}</span>
   <div class="bar" style="flex:1"><i style="width:${100*r.speedup/mx}%"></i></div>
   <b style="width:70px;text-align:right">${r.speedup.toFixed(2)}×</b></div></div>`).join('');}
}
function loadHist(i){fetch('/api/state').then(r=>r.json()).then(s=>{
 const r=s.history.find(x=>x.index===i);if(!r)return;LAST=r;showLast(r);
 document.querySelectorAll('.tabs button').forEach(x=>x.classList.remove('on'));
 document.querySelector('[data-t=code]').classList.add('on');
 document.querySelectorAll('main section').forEach(s=>s.style.display='none');
 $('code').style.display='';});}
async function clearCache(){await api('/api/cache/clear',{method:'POST'});refresh('cache');}
refreshAll();
</script></body></html>
)HTML";

void writeAll(int fd, const std::string &s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return;
        off += (size_t)n;
    }
}

} // namespace

WebServer::WebServer(Engine &engine, int port)
    : engine_(engine), port_(port) {}

WebServer::~WebServer() { stop(); }

void WebServer::start() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port_);
    if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) != 0)
        throw std::runtime_error("bind port " + std::to_string(port_) +
                                 " failed");
    listen(listenFd_, 64);
    // resolve actual port if 0 was requested
    socklen_t al = sizeof(addr);
    getsockname(listenFd_, (sockaddr*)&addr, &al);
    port_ = ntohs(addr.sin_port);
    running_ = true;
    acceptThread_ = std::thread([this] { acceptLoop(); });
}

void WebServer::stop() {
    if (!running_.exchange(false)) return;
    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) acceptThread_.join();
}

void WebServer::acceptLoop() {
    while (running_) {
        int fd = accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (!running_) break;
            continue;
        }
        try { handle(fd); } catch (...) {}
        close(fd);
    }
}

void WebServer::handle(int fd) {
    // Read the full request: headers first, then the declared body length.
    std::string req;
    char chunk[16384];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos && req.size() < 16u * 1024 * 1024) {
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        req.append(chunk, (size_t)n);
        headerEnd = req.find("\r\n\r\n");
        if (n < (ssize_t)sizeof(chunk)) break; // client flushed; body follows
    }
    if (req.empty()) return;
    headerEnd = req.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        size_t contentLength = 0;
        auto cl = req.find("Content-Length:");
        if (cl == std::string::npos) cl = req.find("content-length:");
        if (cl != std::string::npos) {
            contentLength = (size_t)std::stoul(req.substr(cl + 15));
        }
        size_t have = req.size() - (headerEnd + 4);
        while (have < contentLength) {
            ssize_t n = recv(fd, chunk,
                             std::min(sizeof(chunk), contentLength - have), 0);
            if (n <= 0) break;
            req.append(chunk, (size_t)n);
            have += (size_t)n;
        }
    }
    std::istringstream is(req);
    std::string method, path, ver;
    is >> method >> path >> ver;

    auto send = [&](int code, const std::string &ctype,
                    const std::string &body) {
        std::ostringstream o;
        o << "HTTP/1.1 " << code << " OK\r\n"
          << "Content-Type: " << ctype << "\r\n"
          << "Content-Length: " << body.size() << "\r\n"
          << "Connection: close\r\n\r\n" << body;
        writeAll(fd, o.str());
    };
    auto json = [&](const std::string &body) {
        send(200, "application/json; charset=utf-8", body);
    };

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        send(200, "text/html; charset=utf-8", DASHBOARD_HTML);
        return;
    }

    if (method == "GET" && path == "/api/state") {
        const auto &hist = engine_.history();
        std::ostringstream o;
        o << "{";
        const Table &t = engine_.table();
        o << "\"table\":{\"nrows\":" << t.nrows
          << ",\"byteText\":" << jstr(human_bytes(t.byteSize()))
          << ",\"cols\":[";
        for (size_t i = 0; i < t.cols.size(); ++i) {
            if (i) o << ",";
            o << "{\"name\":" << jstr(t.cols[i]->name)
              << ",\"type\":" << jstr(dtype_name(t.cols[i]->type))
              << ",\"distinct\":" << t.cols[i]->distinct << "}";
        }
        o << "]},";
        auto js = engine_.jit().stats();
        o << "\"jit\":{\"lookups\":" << js.lookups
          << ",\"memHits\":" << js.memHits
          << ",\"diskHits\":" << js.diskHits
          << ",\"compiles\":" << js.compiles
          << ",\"fallbacks\":" << js.fallbacks
          << ",\"avgCompileMs\":" << js.avgCompileMs()
          << ",\"savedDisk\":" << js.totalSavedMsDisk
          << ",\"savedMem\":" << js.totalSavedMsMem
          << ",\"compiler\":" << jstr(engine_.jit().compilerFingerprint())
          << "},";
        EngineSummary sm = engine_.summary();
        o << "\"summary\":{\"totalQueries\":" << sm.totalQueries
          << ",\"geoMeanSpeedup\":" << sm.geoMeanSpeedup
          << ",\"totalInterpMs\":" << sm.totalInterpMs
          << ",\"totalJitMs\":" << sm.totalJitMs
          << ",\"totalCompileMs\":" << sm.totalCompileMs
          << ",\"compileSharePct\":" << sm.compileSharePct
          << ",\"matched\":" << sm.matched
          << ",\"mismatched\":" << sm.mismatched << "},";
        o << "\"disk\":" << jstr("缓存目录: " + engine_.jit().cacheDir())
          << ",";
        o << "\"history\":[";
        bool first = true;
        for (auto &r : hist) {
            if (!first) o << ",";
            first = false;
            o << resultJson(r);
        }
        o << "]}";
        json(o.str());
        return;
    }

    if (method == "POST" && path == "/api/query") {
        auto bodyPos = req.find("\r\n\r\n");
        std::string body = bodyPos == std::string::npos ? ""
                                                        : req.substr(bodyPos + 4);
        JsonVal jv = parse_json_object(body);
        std::string sql = jv.get("sql");
        bool verify = jv.get("verify", "true") != "false";
        if (sql.empty()) {
            json("{\"error\":\"empty sql\"}");
            return;
        }
        try {
            QueryResult r = engine_.runSql(sql, verify);
            json(std::string("{\"result\":") + resultJson(r) + "}");
        } catch (const std::exception &e) {
            json(std::string("{\"error\":") + jstr(e.what()) + "}");
        }
        return;
    }

    if (method == "POST" && path == "/api/cache/clear") {
        engine_.jit().clearDiskCache();
        json("{\"ok\":true}");
        return;
    }

    send(404, "text/plain", "not found");
}

} // namespace qex
