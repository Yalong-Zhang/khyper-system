import asyncio,json
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from backend.config import load_config
from backend.datasets import descriptor
from backend.graph_worker import GraphWorker

def fixture(tmp_path,directed=True,edges=None,tsv=False):
 ids=[10,20,614613821311819618,40]
 if edges is None:edges=[(10,20,7),(20,10,2),(ids[2],10,5)] if directed else [(10,20,7),(ids[2],10,5)]
 v={'vid':pa.array(ids,type=pa.int64()),'title':['Alpha','Beta','中文节点','Isolated']}
 e={k:pa.array([row[i] for row in edges],type=pa.int64()) for i,k in enumerate(['u','v','w'])}
 vt={'path':'nodes.parquet','format':'parquet','columns':{'id':'vid','name':'title'}}
 et={'path':'links.parquet','format':'parquet','columns':{'source':'u','target':'v','weight':'w'},'weight_semantics':'test relation counts'}
 if tsv:
  (tmp_path/'nodes.tsv').write_text('vid\ttitle\n'+''.join(f'{i}\t{n}\n' for i,n in zip(ids,v['title'])),encoding='utf-8')
  (tmp_path/'links.tsv').write_text('u\tv\tw\n'+''.join(f'{u}\t{v}\t{w}\n' for u,v,w in edges),encoding='utf-8')
  vt.update(path='nodes.tsv',format='tsv',types={'vid':'int64','title':'string'})
  et.update(path='links.tsv',format='tsv',types={'u':'int64','v':'int64','w':'int64'})
 else:
  pq.write_table(pa.table(v),tmp_path/'nodes.parquet');pq.write_table(pa.table(e),tmp_path/'links.parquet')
 p=tmp_path/'dataset.json';p.write_text(json.dumps({'format':'khyper.graph.v1','id':'test','name':'测试图','directed':directed,'vertices':vt,'edges':et}),encoding='utf-8')
 return p

@pytest.mark.parametrize('directed,tsv',[(True,False),(False,False),(False,True)])
def test_direct_loading_and_exact_ids(tmp_path,directed,tsv):
 path=fixture(tmp_path,directed=directed,tsv=tsv);before={p.name:p.read_bytes() for p in tmp_path.iterdir()}
 async def run():
  w=GraphWorker(load_config())
  try:
   await w.load(descriptor(str(path)))
   assert w.ready and w.info['vertices']==4
   r=await w.call('get_vertex_stats_unweighted',{'vertex_id':'10'})
   if directed:assert (r['in_degree'],r['out_degree'],r['unique_partners'])==(2,1,2)
   else:assert r['degree']==2
   r=await w.call('search_vertices',{'query':'中文'})
   assert r['rows'][0]['vertex_id']=='614613821311819618'
   r=await w.call('get_vertex_neighbors_weighted',{'vertex_id':'10','direction':'in' if directed else 'both'})
   assert r['total_neighbors']==2
   if not directed:
    with pytest.raises(ValueError):await w.call('rank_vertices_weighted',{'metric':'inflow'})
   r=await w.call('get_vertex_stats_weighted',{'vertex_id':'40'})
   assert r.get('strength',r.get('total'))==0
  finally:await w.stop()
 asyncio.run(run())
 assert before=={p.name:p.read_bytes() for p in tmp_path.iterdir()}

@pytest.mark.parametrize('edges,directed',[([(10,10,1)],True), ([(10,20,1),(10,20,2)],True), ([(10,20,1),(20,10,2)],False), ([(10,999,1)],True)])
def test_invalid_graph_rejected_without_partial_state(tmp_path,edges,directed):
 path=fixture(tmp_path,directed=directed,edges=edges)
 async def run():
  w=GraphWorker(load_config())
  try:
   with pytest.raises(Exception):await w.load(descriptor(str(path)))
   assert not w.ready and not w.info and w.state=='error'
  finally:await w.stop()
 asyncio.run(run())

def test_descriptor_rejects_unknown_fields(tmp_path):
 p=fixture(tmp_path);d=json.loads(p.read_text());d['guess_direction']=True;p.write_text(json.dumps(d))
 with pytest.raises(ValueError):descriptor(str(p))


def test_implicit_unit_weights(tmp_path):
 p=fixture(tmp_path,directed=False)
 d=json.loads(p.read_text());d['edges']['columns'].pop('weight');d['edges']['weight_semantics']='one per unweighted edge';p.write_text(json.dumps(d))
 async def run():
  w=GraphWorker(load_config())
  try:
   await w.load(descriptor(str(p)))
   r=await w.call('get_vertex_stats_weighted',{'vertex_id':'10'})
   assert r['strength']==2
  finally:await w.stop()
 asyncio.run(run())
