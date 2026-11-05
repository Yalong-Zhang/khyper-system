"use strict";
const $ = (id) => document.getElementById(id);
let selectedDataset = null, healthTimer = null;
let ready = false, running = false, controller = null, reportMarkdown = "", startedAt = 0, evidenceCount = 0;
const steps = new Map();
const icon = (name) => `<svg aria-hidden="true"><use href="#i-${name}"/></svg>`;
const escapeHTML = (value) => String(value).replace(/[&<>"']/g, (c) => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const number = (value) => typeof value === "number" ? value.toLocaleString("zh-CN", {maximumFractionDigits: 3}) : (value ?? "—");
const labels = {field:"字段",query_field:"查询字段",column:"物理列",type:"类型",nullable:"允许空值",implicit:"隐式字段",null_count:"空值数",non_null_count:"非空数",null_fraction:"空值比例",examples:"取值示例",id:"节点 ID",source:"source 节点 ID",target:"target 节点 ID",degree:"度数",strength:"强度",vertex_id:"节点 ID",name:"节点",rank:"排名",value:"指标值",weight:"权重",share:"占比",inflow:"入强度",outflow:"出强度",total:"总关联权重",net_inflow:"净入强度",in_degree:"不同来源数",out_degree:"不同去向数",total_degree:"总度",unique_partners:"不同关联节点数",hq_country:"总部国家",rics_k50:"行业",exact_match:"精确匹配"};

function inline(text) {
  return escapeHTML(text).replace(/`([^`]+)`/g,"<code>$1</code>").replace(/\*\*([^*]+)\*\*/g,"<strong>$1</strong>").replace(/\*([^*]+)\*/g,"<em>$1</em>");
}
function markdown(text) {
  // Deliberately small renderer. All model text is escaped; raw HTML and links
  // are never interpreted. Tables, lists and headings cover report formatting.
  const lines = text.replace(/\r/g, "").split("\n");
  let html = "", list = null;
  const closeList = () => { if (list) {html += `</${list}>`;list = null;} };
  for (let i=0; i<lines.length; i++) {
    const line = lines[i].trim();
    if (!line) {closeList();continue;}
    if (line.startsWith("|") && i+1 < lines.length && /^\|?\s*:?-{3}/.test(lines[i+1].trim())) {
      closeList();
      const split = (s) => s.trim().replace(/^\|/, "").replace(/\|$/, "").split("|").map((x)=>x.trim());
      const header = split(line);i++;
      html += '<div class="table-wrap"><table><thead><tr>'+header.map(x=>`<th>${inline(x)}</th>`).join("")+"</tr></thead><tbody>";
      while(i+1 < lines.length && lines[i+1].trim().startsWith("|")) html += "<tr>"+split(lines[++i]).map(x=>`<td>${inline(x)}</td>`).join("")+"</tr>";
      html += "</tbody></table></div>";continue;
    }
    const heading = /^(#{1,3})\s+(.+)$/.exec(line);
    if (heading) {closeList();const n=heading[1].length;html += `<h${n}>${inline(heading[2])}</h${n}>`;continue;}
    const item = /^(?:([-*])\s+|\d+\.\s+)(.+)$/.exec(line);
    if (item) {const type=item[1]?"ul":"ol";if(list!==type){closeList();list=type;html+=`<${type}>`;}html+=`<li>${inline(item[2])}</li>`;continue;}
    closeList();
    if (/^---+$/.test(line)) html+="<hr>";
    else if (line.startsWith(">")) html+=`<blockquote>${inline(line.slice(1).trim())}</blockquote>`;
    else if (!line.startsWith("```")) html+=`<p>${inline(line)}</p>`;
  }
  closeList();return html;
}
function updateSubmit() {$('submit-button').disabled = !ready || running || !$('question').value.trim();}
function notice(message) {$('notice').textContent=message;$('notice').classList.toggle('hidden',!message);}
function setPill(label, state="") {$('progress-pill').textContent=label;$('progress-pill').className=`progress-pill ${state}`;}
function finishSteps(state) {
  for (const [id, li] of steps) if(li.classList.contains('running')) changeStep(id,state,state==='error'?'本次步骤未完成':'已停止');
}
function changeStep(id, state, detail="", label="") {
  let li=steps.get(id);
  if (!li) {
    li=document.createElement('li');li.dataset.step=id;
    li.innerHTML='<span class="step-icon"></span><div class="step-text"><span class="step-label"></span><span class="step-detail"></span></div>';
    $('step-list').append(li);steps.set(id,li);
  }
  li.className=state;
  if(label)li.querySelector('.step-label').textContent=label;
  li.querySelector('.step-detail').textContent=detail;
  li.querySelector('.step-icon').innerHTML=state==='done'?icon('check'):state==='error'?'!':'';
  if(state==='running') $('current-action').textContent=label;
}
function table(rows, metric, tool) {
  if (!rows.length) return '<p class="evidence-note">未找到符合条件的数据。</p>';
  let cols=Object.keys(rows[0]);
  if(tool==='search_vertices')cols=cols.filter(k=>!['exact_match','match_type','alias_note','properties'].includes(k));
  return '<div class="evidence-table-wrap"><table><thead><tr>'+cols.map(c=>`<th>${escapeHTML(c==='value'?(labels[metric]||'指标值'):(labels[c]||c))}</th>`).join('')+'</tr></thead><tbody>'+rows.map(row=>'<tr>'+cols.map(c=>`<td>${escapeHTML(c==='share'&&row[c]!=null?(row[c]*100).toFixed(2)+'%':(row[c]!==null && typeof row[c]==="object"?JSON.stringify(row[c]):number(row[c])))}</td>`).join('')+'</tr>').join('')+'</tbody></table></div>';
}
function addEvidence(event) {
  evidenceCount++;$('evidence-count').textContent=`${evidenceCount} 项`;
  $('evidence-section').classList.remove('hidden');
  const details=document.createElement('details');
  const summary=document.createElement('summary');summary.textContent=event.label;details.append(summary);
  const body=document.createElement('div');body.className='evidence-body';
  const data=event.data;
  if(data.rows){
    body.innerHTML=table(data.rows,data.metric,event.tool);
    const total=data.matched_neighbors??data.total_matches??data.matched_vertices??data.total_groups??data.total_vertices;
    const note=document.createElement('p');note.className='evidence-note';
    note.textContent=`显示 ${data.rows.length} 项${total!=null?` / 共 ${number(total)} 项`:''}${data.truncated?'，其余未展示':''}。`;
    if(data.total_weight!=null)note.textContent+=` 该方向全部权重：${number(data.total_weight)}。`;
    if(data.matched_weight!=null)note.textContent+=` 筛选后权重：${number(data.matched_weight)}。占比分母仍为完整方向权重。`;
    if(data.entity==='edges' && data.endpoint_order==='canonical_external_id_ascending')note.textContent+=' 无向边端点按外部 ID 从小到大排列，每条边只计一次。';
    if(data.scope)note.textContent+=' 统计基于全部匹配记录；排名指标在完整图上计算。';
    body.append(note);
  }else{
    const values=Object.entries(data).filter(([k])=>labels[k]);
    if(values.length)body.innerHTML='<div class="evidence-kv">'+values.map(([k,v])=>`<span>${escapeHTML(labels[k])}<strong>${escapeHTML(number(v))}</strong></span>`).join('')+'</div>';
    else body.textContent=`数据集：${data.dataset||'节点图网络'}，${number(data.vertices)} 个节点，${number(data.edges)} 条关系。`;
  }
  details.append(body);$('evidence-list').append(details);
}
function handleEvent(event) {
  if(event.type==='step_start') changeStep(event.id,'running','',event.label);
  else if(event.type==='step_done') changeStep(event.id,'done',event.summary||'已完成');
  else if(event.type==='step_error') changeStep(event.id,'error',event.message,event.label);
  else if(event.type==='evidence') addEvidence(event);
  else if(event.type==='report') {
    reportMarkdown=event.markdown;
    $('report').innerHTML=markdown(reportMarkdown);
    $('report').classList.remove('hidden');$('report-loading').classList.add('hidden');$('copy-button').classList.remove('hidden');
    $('report-meta').textContent=`本次分析 · ${Math.max(1,Math.round((Date.now()-startedAt)/1000))} 秒 · ${event.tool_calls} 次统计工具调用 · ${event.model_calls} 次模型请求${event.total_tokens?` · ${number(event.total_tokens)} tokens`:''}${event.has_evidence?'':' · 本次未执行图统计，仅说明支持范围或数据限制'}`;
    $('report-meta').classList.remove('hidden');setPill('已完成','done');
  } else if(event.type==='error') {finishSteps('error');notice(event.message);setPill('未完成','error');$('report-loading').classList.add('hidden');}
}

async function submit(event) {
  event?.preventDefault();
  if(running||!ready||!$('question').value.trim())return;
  running=true;startedAt=Date.now();evidenceCount=0;reportMarkdown='';steps.clear();
  $('step-list').replaceChildren();$('evidence-list').replaceChildren();
  for(const id of ['report','report-empty','report-meta','copy-button','evidence-section','progress-intro'])$(id).classList.add('hidden');
  $('report-loading').classList.remove('hidden');$('cancel-button').classList.remove('hidden');notice('');setPill('分析中','running');
  $('submit-button').innerHTML='正在分析<span class="spinner button-spinner"></span>';
  $('question').disabled=true;document.querySelectorAll('[data-question]').forEach(b=>b.disabled=true);updateSubmit();
  controller=new AbortController();
  let reportReceived=false, failureReceived=false;
  try {
    const response=await fetch('/api/analyze',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({question:$('question').value.trim(),dataset_id:selectedDataset}),signal:controller.signal});
    if(!response.ok){let message='分析请求未能开始。';try{const e=await response.json();message=typeof e.detail==='string'?e.detail:message;}catch{}throw new Error(message);}
    const reader=response.body.getReader(), decoder=new TextDecoder();let pending='';
    while(true){
      const {value,done}=await reader.read();pending+=decoder.decode(value||new Uint8Array(),{stream:!done});
      let split;
      while((split=pending.indexOf('\n'))>=0){const line=pending.slice(0,split);pending=pending.slice(split+1);if(!line.trim())continue;const e=JSON.parse(line);handleEvent(e);if(e.type==='report')reportReceived=true;if(e.type==='error')failureReceived=true;}
      if(done)break;
    }
    if(!reportReceived&&!failureReceived)throw new Error('连接已结束，但尚未收到完整报告。');
  }catch(error){
    finishSteps('error');setPill(error.name==='AbortError'?'已停止':'未完成','error');
    notice(error.name==='AbortError'?'已停止本次分析，已完成的统计仍可在数据依据中查看。':error.message||'连接中断，请检查本地服务。');
    $('report-loading').classList.add('hidden');
  }finally{
    running=false;controller=null;$('question').disabled=false;document.querySelectorAll('[data-question]').forEach(b=>b.disabled=false);
    $('cancel-button').classList.add('hidden');$('submit-button').innerHTML='开始分析'+icon('arrow');updateSubmit();
    if(!reportReceived){$('report-empty').classList.remove('hidden');$('report-empty').querySelector('h3').textContent='本次分析尚未形成报告';$('report-empty').querySelector('p').textContent='你可以调整问题后，重新开始一次分析。';}
  }
}
async function pollHealth(){
  clearTimeout(healthTimer);
  try {
    const response=await fetch('/api/health',{cache:'no-store'});if(!response.ok)throw new Error();const h=await response.json();
    ready=h.status==='ready'&&h.model_configured;
    selectedDataset=h.status==='ready'?h.dataset.id:null;
    $('load-dataset').disabled=h.busy||running;
    $('dataset-state').textContent=h.status==='ready'?('已加载：'+h.dataset.name+' · '+(h.dataset.directed?'有向图':'无向图')):h.status==='loading'?('正在加载：'+({starting:'启动工具',vertices:'读取节点',edges:'读取边',validating:'校验结构'}[h.progress?.phase]||'准备中')+' · 已读取 '+number(h.progress?.records||0)+' 条记录'):h.error||'请选择数据集描述文件';
    $('dataset-name').textContent=h.dataset.name||'尚未选择数据集';
    $('health-badge').className='status-badge '+(ready?'':h.status==='error'||!h.model_configured?'error':'loading');
    $('health-label').textContent=ready?'数据已就绪':h.status==='error'?'数据加载失败':!h.model_configured?'模型尚未配置':h.status==='empty'?'尚未选择数据集':'正在加载数据';
    const d=h.dataset||{};
    if(d.vertices!=null)$('stat-vertices').innerHTML=escapeHTML(number(d.vertices))+'<span>个节点</span>';
    if(d.edges!=null)$('stat-edges').innerHTML=escapeHTML(number(d.edges))+'<span>条记录</span>';
    if(h.model)$('model-label').textContent=h.model+' · 基础图统计';
    if(h.error&&!running)notice(h.error);
    if(!h.model_configured&&!running)notice('请先在本地配置模型接口、模型名称和 API key，然后重启服务。');
  }catch{
    ready=false;$('health-badge').className='status-badge error';$('health-label').textContent='服务连接中断';
  }finally{updateSubmit();healthTimer=setTimeout(pollHealth,hReadyDelay());}
}
$('question-form').addEventListener('submit',submit);
$('question').addEventListener('input',updateSubmit);
$('question').addEventListener('keydown',e=>{if((e.ctrlKey||e.metaKey)&&e.key==='Enter'){e.preventDefault();submit();}});
document.querySelectorAll('[data-question]').forEach(button=>button.addEventListener('click',()=>{$('question').value=button.dataset.question;$('question').focus();updateSubmit();}));
$('cancel-button').addEventListener('click',()=>controller?.abort());
$('about-button').addEventListener('click',()=>$('about-dialog').showModal());
$('close-dialog').addEventListener('click',()=>$('about-dialog').close());
$('about-dialog').addEventListener('click',e=>{if(e.target===$('about-dialog')){const r=e.target.getBoundingClientRect();if(e.clientX<r.left||e.clientX>r.right||e.clientY<r.top||e.clientY>r.bottom)e.target.close();}});
$('copy-button').addEventListener('click',async()=>{try{await navigator.clipboard.writeText(reportMarkdown);$('copy-button').querySelector('span').textContent='已复制';setTimeout(()=>$('copy-button').querySelector('span').textContent='复制报告',1800);}catch{notice('复制失败，请直接选中报告文本复制。');}});
function hReadyDelay(){return running?3000:ready?8000:1500;}
$('browse-dataset').addEventListener('click',async()=>{
  const button=$('browse-dataset');button.disabled=true;button.textContent='正在选择…';
  try {const response=await fetch('/api/datasets/pick',{method:'POST'});const data=await response.json();if(!response.ok)throw new Error(data.detail);if(data.path)$('dataset-path').value=data.path;}
  catch(e){notice(e.message);}finally{button.disabled=false;button.textContent='浏览路径…';}
});
$('load-dataset').addEventListener('click',async()=>{
  $('load-dataset').disabled=true;notice('');
  try{
    const response=await fetch('/api/datasets/load',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path:$('dataset-path').value.trim()})});
    const data=await response.json();if(!response.ok)throw new Error(typeof data.detail==='string'?data.detail:'请选择描述文件。');
    ready=false;selectedDataset=null;steps.clear();$('step-list').replaceChildren();$('evidence-list').replaceChildren();evidenceCount=0;reportMarkdown='';
    for(const id of ['report','report-meta','copy-button','evidence-section'])$(id).classList.add('hidden');
    $('report-empty').classList.remove('hidden');$('stat-vertices').textContent='—';$('stat-edges').textContent='—';setPill('等待加载');
    await pollHealth();
  }catch(e){notice(e.message);$('load-dataset').disabled=false;}
  updateSubmit();
});
fetch('/api/datasets/examples').then(r=>r.json()).then(items=>{for(const item of items){const b=document.createElement('button');b.type='button';b.textContent=item.name;b.addEventListener('click',()=>{$('dataset-path').value=item.path;});$('dataset-examples').append(b);}}).catch(()=>{});
pollHealth();updateSubmit();
