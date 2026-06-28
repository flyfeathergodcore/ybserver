// ═══════════════════════════════════════════════════════════════
// Dashboard — real-time HTTP server metrics via SSE
// ═══════════════════════════════════════════════════════════════

const MAX=60;
const labels=[],qpsData=[],errData=[],p50Data=[],p90Data=[],p99Data=[],actData=[];

const common={responsive:true,maintainAspectRatio:false,animation:false,resize:{delay:0},
  scales:{x:{display:false},y:{beginAtZero:true,grid:{color:'#2a2a4e'}}},
  plugins:{legend:{labels:{color:'#aaa',boxWidth:12,font:{size:11}}}}};

const QPS=new Chart(document.getElementById('chart-qps'),{type:'line',data:{
  labels,datasets:[
    {label:'QPS',data:qpsData,borderColor:'#53d769',backgroundColor:'rgba(83,215,105,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'Errors',data:errData,borderColor:'#e94560',backgroundColor:'rgba(233,69,96,0.1)',fill:true,pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:common});

const LAT=new Chart(document.getElementById('chart-latency'),{type:'line',data:{
  labels,datasets:[
    {label:'p50',data:p50Data,borderColor:'#53d769',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p90',data:p90Data,borderColor:'#ffa726',pointRadius:0,borderWidth:1.5,tension:.3},
    {label:'p99',data:p99Data,borderColor:'#e94560',pointRadius:0,borderWidth:1.5,tension:.3}
  ]},options:common});

const ACT=new Chart(document.getElementById('chart-connections'),{type:'line',data:{
  labels,datasets:[
    {label:'Active',data:actData,borderColor:'#42a5f5',backgroundColor:'rgba(66,165,245,0.15)',fill:true,pointRadius:0,borderWidth:2,tension:.3}
  ]},options:common});

// ── Helpers ──

function pushData(pt){
  labels.push(labels.length+'s');
  qpsData.push(pt.qps);errData.push(pt.err);
  p50Data.push(pt.p50);p90Data.push(pt.p90);p99Data.push(pt.p99);
  actData.push(pt.act);
  if(labels.length>MAX){ labels.shift();qpsData.shift();errData.shift();
    p50Data.shift();p90Data.shift();p99Data.shift();actData.shift(); }
  QPS.update();LAT.update();ACT.update();
}

function updateSummary(pt){
  document.getElementById('cur-qps').textContent=pt.qps;
  document.getElementById('cur-err').textContent=pt.err;
  document.getElementById('cur-lat').textContent=pt.p50+'/'+pt.p90+'/'+pt.p99+'µs';
  document.getElementById('cur-act').textContent=pt.act;
}

function updateAlerts(alerts){
  const panel=document.getElementById('alert-panel');
  if(!alerts||!alerts.length){panel.style.display='none';return;}
  panel.style.display='block';
  panel.innerHTML='';
  for(const a of alerts){
    const div=document.createElement('div');
    div.className='alert-item'+(a.state==='ok'?' ok':'');
    div.innerHTML='<span class="a-name">'+a.name+'</span>'+
      '<span class="a-state '+(a.state==='firing'?'firing':'ok')+'">'+
      a.state+'</span>';
    panel.appendChild(div);
  }
}

// ── Uptime counter (updates locally every second) ──

let uptimeStart = 0;
let lastSummary = null;

function tickUptime(){
  if(!uptimeStart)return;
  const el=document.getElementById('cur-uptime');
  const s=Math.floor(Date.now()/1000-uptimeStart);
  const h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sec=s%60;
  el.textContent=h+':'+String(m).padStart(2,'0')+':'+String(sec).padStart(2,'0');
}
setInterval(tickUptime,1000);

// ── SSE connection ──

let sinceTs=0;
const es=new EventSource('/metrics/stream');

es.addEventListener('full',function(e){
  try{
    const d=JSON.parse(e.data);
    uptimeStart = Math.floor(Date.now()/1000) - (d.uptime_seconds||0);
    tickUptime();

    const h=d.history||[];
    sinceTs=h.length>0?h[h.length-1].t:0;
    for(let i=0;i<h.length;i++)pushData(h[i]);
    if(h.length>0){
      lastSummary=h[h.length-1];
      updateSummary(lastSummary);
    }
    updateAlerts(d.alerts);
  }catch(x){console.error(x);}
});

es.addEventListener('metrics',function(e){
  try{
    const pt=JSON.parse(e.data);
    pushData(pt);
    if(pt.t>sinceTs)sinceTs=pt.t;
    // Only update summary when there's actual traffic data
    if(pt.qps>0||pt.err>0||pt.p50>0){
      lastSummary=pt;
      updateSummary(pt);
    }
  }catch(x){console.error(x);}
});

es.addEventListener('alert',function(e){
  try{
    const a=JSON.parse(e.data);
    fetch('/metrics.json').then(r=>r.json()).then(d=>updateAlerts(d.alerts)).catch(()=>{});
  }catch(x){console.error(x);}
});
