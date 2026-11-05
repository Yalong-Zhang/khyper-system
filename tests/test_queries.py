import asyncio,json
from pathlib import Path
import pyarrow as pa
import pyarrow.parquet as pq
import pytest
from backend.config import load_config
from backend.datasets import descriptor
from backend.graph_worker import GraphWorker
from backend.tools import REFERENCE, REGISTRY, render_tool_reference, tools_for, validate_arguments

BIG=9007199254740993

def dataset(tmp_path,directed=True):
 ids=[10,20,30,40,BIG]
 pq.write_table(pa.table({'id':pa.array(ids,type=pa.int64()),'name':[f'N{i}' for i in ids],
  'group':pa.array(['A','A','B',None,'A'],type=pa.string()),'score':pa.array([5,None,-3,5,7],type=pa.int64()),
  'flag':pa.array([True,False,None,True,False],type=pa.bool_())}),tmp_path/'v.parquet')
 edges=[(10,20,7),(20,10,2),(30,10,5),(BIG,30,0),(40,BIG,3),(10,30,4)] if directed else [(20,10,7),(30,10,5),(BIG,30,0),(40,BIG,3)]
 pq.write_table(pa.table({**{name:pa.array([edge[i] for edge in edges],type=pa.int64()) for i,name in enumerate(('s','t','w'))},
  'kind':pa.array(['x','x','y',None,'y','x'][:len(edges)],type=pa.string()),
  'cost':pa.array([1.5,None,2.5,0,-1,0.25][:len(edges)],type=pa.float64()),
  'odd':['literal']*len(edges)}),tmp_path/'e.parquet')
 d={'format':'khyper.graph.v1','id':'query-fixture','name':'Generic query fixture','directed':directed,
 'vertices':{'path':'v.parquet','format':'parquet','columns':{'id':'id','name':'name'},'properties':{'group':'group','score':'score','flag':'flag'}},
 'edges':{'path':'e.parquet','format':'parquet','columns':{'source':'s','target':'t','weight':'w'},'properties':{'kind':'kind','cost':'cost','source.group':'odd'},'weight_semantics':'integer relation count'}}
 path=tmp_path/'dataset.json';path.write_text(json.dumps(d),encoding='utf-8');return path


def run(path,check):
 async def main():
  worker=GraphWorker(load_config())
  try:await worker.load(descriptor(str(path)));await check(worker)
  finally:await worker.stop()
 asyncio.run(main())


def test_schema_profiles_and_selected_queries(tmp_path):
 async def check(w):
  schema=await w.call('get_dataset_schema',{'entity':'vertices'})
  fields={row['field']:row for row in schema['rows']}
  assert fields['score']['type']=='int64' and fields['flag']['type']=='bool'
  assert fields['id']['nullable'] is False and fields['score']['nullable'] is True
  profile=await w.call('profile_fields',{'entity':'vertices','fields':['group','score','id'],'example_limit':3})
  p={row['field']:row for row in profile['rows']}
  assert p['group']['null_count']==1 and p['score']['non_null_count']==4
  assert p['score']['examples']==[5,-3,7] and p['id']['examples']==['10','20','30']
  q=await w.call('query_vertices',{'fields':['id','score'],'filter':[{'field':'group','op':'in','value':['A','B']}], 'order_by':[{'field':'score','direction':'desc'}],'limit':3})
  assert q['total_matches']==4 and q['truncated']
  assert q['rows']==[{'id':str(BIG),'score':7},{'id':'10','score':5},{'id':'30','score':-3}]
  q=await w.call('query_vertices',{'fields':['id'],'filter':[{'field':'id','op':'eq','value':str(BIG)}]})
  assert q['rows']==[{'id':str(BIG)}]
  # ne excludes nulls; it is not a substitute for is_not_null.
  q=await w.call('query_vertices',{'filter':[{'field':'group','op':'ne','value':'A'}],'fields':['id']})
  assert q['rows']==[{'id':'30'}]
  q=await w.call('query_vertices',{'filter':[{'field':'flag','op':'is_null'}],'fields':['id']})
  assert q['rows']==[{'id':'30'}]
 run(dataset(tmp_path),check)


def test_group_aggregation_uses_all_matches_and_null_groups(tmp_path):
 async def check(w):
  aggs=[{'op':'count','as':'n'},{'op':'count','field':'score','as':'known'}, {'op':'sum','field':'score','as':'sum'}, {'op':'avg','field':'score','as':'mean'}, {'op':'min','field':'score','as':'min'}, {'op':'max','field':'score','as':'max'}]
  result=await w.call('aggregate_vertices',{'group_by':['group'],'aggregations':aggs,'order_by':[{'field':'n','direction':'desc'}],'limit':1})
  assert result['matched_records']==5 and result['total_groups']==3 and result['truncated']
  assert result['rows']==[{'group':'A','n':3,'known':2,'sum':12,'mean':6.0,'min':5,'max':7}]
  result=await w.call('aggregate_vertices',{'group_by':['group'],'aggregations':aggs,'limit':10})
  assert [r['group'] for r in result['rows']]==['A','B',None]
  result=await w.call('aggregate_vertices',{'filter':[{'field':'id','op':'eq','value':'20'}],'aggregations':aggs})
  assert result['rows']==[{'n':1,'known':0,'sum':0,'mean':None,'min':None,'max':None}]
  result=await w.call('aggregate_vertices',{'filter':[{'field':'group','op':'eq','value':'absent'}],'aggregations':aggs})
  assert result['rows'][0]=={'n':0,'known':0,'sum':0,'mean':None,'min':None,'max':None}
  result=await w.call('aggregate_vertices',{'filter':[{'field':'group','op':'eq','value':'absent'}],'group_by':['group'],'aggregations':[{'op':'count','as':'n'}]})
  assert result['rows']==[] and result['total_groups']==0
 run(dataset(tmp_path),check)


def test_endpoint_filters_edge_attributes_and_directed_groups(tmp_path):
 async def check(w):
  defaults=await w.call('query_edges',{'limit':1})
  assert defaults['rows'][0]['edge.source.group']=='literal'
  prof=await w.call('profile_fields',{'entity':'edges'})
  assert next(row for row in prof['rows'] if row['field']=='edge.source.group')['examples']==['literal']
  result=await w.call('query_edges',{'source_filter':[{'field':'group','op':'eq','value':'A'}], 'target_filter':[{'field':'group','op':'eq','value':'B'}], 'fields':['source','target','weight','edge.source.group'], 'order_by':[{'field':'weight','direction':'desc'}]})
  assert result['rows']==[{'source':'10','target':'30','weight':4,'edge.source.group':'literal'}, {'source':str(BIG),'target':'30','weight':0,'edge.source.group':'literal'}]
  result=await w.call('aggregate_edges',{'group_by':['source.group'],'aggregations':[{'op':'sum','field':'weight','as':'amount'},{'op':'count','as':'n'}],'order_by':[{'field':'amount','direction':'desc'}]})
  assert result['rows']==[{'source.group':'A','amount':13,'n':4},{'source.group':'B','amount':5,'n':1},{'source.group':None,'amount':3,'n':1}]
  result=await w.call('aggregate_edges',{'aggregations':[{'op':'sum','field':'source.score','as':'per_edge_source_score'},{'op':'sum','field':'cost','as':'cost_sum'}]})
  assert result['rows']==[{'per_edge_source_score':19,'cost_sum':3.25}]
 run(dataset(tmp_path),check)


def test_filtered_rankings_and_neighbors_keep_global_metrics(tmp_path):
 async def check(w):
  f=[{'field':'group','op':'eq','value':'A'}]
  result=await w.call('rank_vertices_weighted',{'metric':'total','filter':f,'limit':2})
  assert result['matched_vertices']==3 and result['total_vertices']==5 and result['truncated']
  assert [r['value'] for r in result['rows']]==[18,9]
  result=await w.call('rank_vertices_unweighted',{'metric':'total_degree','filter':f})
  assert [r['value'] for r in result['rows']]==[4,2,2]
  result=await w.call('get_vertex_neighbors_weighted',{'vertex_id':'10','direction':'in','neighbor_filter':f})
  assert result['total_neighbors']==2 and result['matched_neighbors']==1
  assert result['total_weight']==7 and result['matched_weight']==2
  assert result['rows'][0]['vertex_id']=='20' and result['rows'][0]['share']==pytest.approx(2/7)
  result=await w.call('get_vertex_neighbors_unweighted',{'vertex_id':'10','direction':'out','neighbor_filter':f})
  assert result['total_neighbors']==2 and result['matched_neighbors']==1
  assert result['rows'][0]['vertex_id']=='20'
 run(dataset(tmp_path),check)


def test_undirected_edges_are_canonical_and_counted_once(tmp_path):
 async def check(w):
  q=await w.call('query_edges',{'fields':['source','target','weight']})
  assert q['total_matches']==4 and q['endpoint_order']=='canonical_external_id_ascending'
  assert [(r['source'],r['target']) for r in q['rows']]==[('10','20'),('10','30'),('30',str(BIG)),('40',str(BIG))]
  result=await w.call('aggregate_edges',{'aggregations':[{'op':'count','as':'n'},{'op':'sum','field':'weight','as':'sum'}]})
  assert result['rows']==[{'n':4,'sum':15}]
  q=await w.call('query_edges',{'source_filter':[{'field':'group','op':'eq','value':'A'}],'target_filter':[{'field':'group','op':'eq','value':'B'}],'fields':['source','target']})
  assert q['rows']==[{'source':'10','target':'30'}]
  r=await w.call('rank_vertices_weighted',{'metric':'strength','filter':[{'field':'group','op':'eq','value':'A'}]})
  assert [row['value'] for row in r['rows']]==[12,7,3]
 run(dataset(tmp_path,False),check)


@pytest.mark.parametrize('tool,args',[
 ('query_vertices',{'filter':[{'field':'absent','op':'is_null'}]}),
 ('query_vertices',{'filter':[{'field':'group','op':'eq','value':1}]}),
 ('query_vertices',{'filter':[{'field':'score','op':'eq','value':5.0}]}),
 ('query_vertices',{'filter':[{'field':'flag','op':'gt','value':True}]}),
 ('aggregate_vertices',{'aggregations':[{'op':'sum','field':'group','as':'bad'}]}),
 ('query_vertices',{'order_by':[{'field':'missing'}]}),
 ('query_vertices',{'filter':[{'field':'score','op':'eq','value':'9223372036854775808'}]}),
])
def test_native_type_and_field_validation(tmp_path,tool,args):
 async def check(w):
  with pytest.raises(ValueError):await w.call(tool,args)
  assert w.ready
 run(dataset(tmp_path),check)


def test_signed_overflow_and_average_precision(tmp_path):
 path=dataset(tmp_path);table=pq.read_table(tmp_path/'v.parquet').to_pydict();table['score']=pa.array([2**63-1,1,0,0,0],type=pa.int64());pq.write_table(pa.table(table),tmp_path/'v.parquet')
 async def check(w):
  with pytest.raises(ValueError,match='overflow'):await w.call('aggregate_vertices',{'aggregations':[{'op':'sum','field':'score','as':'sum'}]})
  result=await w.call('aggregate_vertices',{'aggregations':[{'op':'avg','field':'score','as':'mean'}]})
  assert result['rows'][0]['mean']==pytest.approx((2**63)/5)
  result=await w.call('aggregate_vertices',{'group_by':['id'],'aggregations':[{'op':'count','as':'n'}],'order_by':[{'field':'id','direction':'desc'}]})
  assert result['rows'][0]['id']==str(BIG)
 run(path,check)


@pytest.mark.parametrize('args',[
 {'filter':[{'field':'score','op':'eq'}]}, {'filter':[{'field':'score','op':'is_null','value':0}]},
 {'filter':[{'field':'score','op':'in','value':1}]}, {'filter':[{'field':'score','op':'eq','value':[1]}]},
 {'filter':[{'field':'score','op':'eq','value':None}]}, {'filter':[{'field':'score','op':'eq','value':1,'extra':True}]},
 {'fields':['id','id']}, {'limit':True}, {'limit':51}, {'filter':[{'field':'score','op':'eq','value':float('nan')}]},
])
def test_recursive_registry_validation(args):
 with pytest.raises(ValueError):validate_arguments('query_vertices',args)


def test_reference_is_generated_and_registry_matches_capabilities():
 assert REFERENCE.read_text(encoding='utf-8')==render_tool_reference()
 assert len(REGISTRY)==14
 assert all(f'### {name}\n' in REFERENCE.read_text(encoding='utf-8') for name in REGISTRY)
 for directed in (True,False):
  available={t['function']['name']:t['function'] for t in tools_for(directed)}
  expected=['in','out'] if directed else ['both']
  assert available['get_vertex_neighbors_weighted']['parameters']['properties']['direction']['enum']==expected
 with pytest.raises(ValueError):validate_arguments('aggregate_vertices',{'group_by':['group'],'aggregations':[{'op':'count','as':'group'}]})
 with pytest.raises(ValueError):validate_arguments('rank_vertices_weighted',{'metric':'total'},False)


def test_new_tools_execute_through_agent_with_mocked_model(tmp_path):
 import httpx
 from backend.agent import analyze
 from backend.llm import ModelClient
 async def main():
  config=load_config();config['llm'].update(api_key='local-fixture',base_url='https://example.test/v1',model='fixture')
  config['agent'].update(max_model_calls=5,max_tool_calls=5)
  worker=GraphWorker(config);requests=[]
  def handler(request):
   body=json.loads(request.content);requests.append(body)
   assert '# Agent tool reference' in body['messages'][0]['content']
   steps=[('get_dataset_schema',{'entity':'vertices'}),('profile_fields',{'entity':'vertices','fields':['group']}),('aggregate_vertices',{'group_by':['group'],'aggregations':[{'op':'count','as':'n'}]})]
   if len(requests)<=len(steps):
    name,args=steps[len(requests)-1]
    msg={'role':'assistant','content':None,'tool_calls':[{'id':f'c{len(requests)}','type':'function','function':{'name':name,'arguments':json.dumps(args)}}]}
   else:
    assert '"n": 3' in body['messages'][-1]['content']
    msg={'role':'assistant','content':'A: 3; B: 1; null: 1.'}
   return httpx.Response(200,json={'choices':[{'message':msg,'finish_reason':'stop'}]})
  try:
   await worker.load(descriptor(str(dataset(tmp_path))))
   client=ModelClient(config['llm'],httpx.MockTransport(handler))
   events=[event async for event in analyze('Count nodes by group',config,worker,client)]
   assert events[-1]['type']=='report' and events[-1]['has_evidence']
   assert [event['tool'] for event in events if event['type']=='evidence']==['get_dataset_schema','profile_fields','aggregate_vertices']
  finally:await worker.stop()
 asyncio.run(main())


def test_numeric_filter_operators_and_null_sort_order(tmp_path):
 async def check(w):
  expected={'eq':[10,40],'ne':[30,BIG],'gt':[BIG],'gte':[10,40,BIG],'lt':[30],'lte':[10,30,40],'in':[30,BIG]}
  for op,ids in expected.items():
   result=await w.call('query_vertices',{'fields':['id'],'filter':[{'field':'score','op':op,'value':['-3','7'] if op=='in' else '5'}]})
   assert [row['id'] for row in result['rows']]==[str(i) for i in ids]
  result=await w.call('query_vertices',{'fields':['id'],'filter':[{'field':'group','op':'eq','value':'A'},{'field':'flag','op':'eq','value':False},{'field':'score','op':'is_not_null'}]})
  assert result['rows']==[{'id':str(BIG)}]
  result=await w.call('query_vertices',{'fields':['id'],'order_by':[{'field':'score','direction':'asc'}]})
  assert [r['id'] for r in result['rows']]==[str(i) for i in (30,10,40,BIG,20)]
  result=await w.call('aggregate_vertices',{'aggregations':[{'op':'min','field':'id','as':'first'},{'op':'max','field':'id','as':'last'}]})
  assert result['rows']==[{'first':'10','last':str(BIG)}]
 run(dataset(tmp_path),check)


def test_group_resource_limit_is_not_silent_result_truncation(tmp_path):
 path=dataset(tmp_path);d=json.loads(path.read_text());d['vertices']['properties']={};d['edges']['properties']={};path.write_text(json.dumps(d))
 n=100001
 pq.write_table(pa.table({'id':pa.array(range(n),type=pa.int64()),'name':['v']*n}),tmp_path/'v.parquet')
 pq.write_table(pa.table({k:pa.array([],type=pa.int64()) for k in ('s','t','w')}),tmp_path/'e.parquet')
 async def check(w):
  with pytest.raises(ValueError,match='group limit exceeded'):
   await w.call('aggregate_vertices',{'group_by':['id'],'aggregations':[{'op':'count','as':'n'}],'limit':1})
  assert w.ready
  result=await w.call('aggregate_vertices',{'aggregations':[{'op':'count','as':'n'}]})
  assert result['rows']==[{'n':n}]
 run(path,check)


def test_float_positive_and_negative_zero_share_one_group(tmp_path):
 path=dataset(tmp_path);table=pq.read_table(tmp_path/'e.parquet').to_pydict();table['cost']=pa.array([-0.0,0.0,-0.0,0.0,-0.0,0.0],type=pa.float64());pq.write_table(pa.table(table),tmp_path/'e.parquet')
 async def check(w):
  result=await w.call('aggregate_edges',{'group_by':['cost'],'aggregations':[{'op':'count','as':'n'}]})
  assert result['total_groups']==1 and result['rows']==[{'cost':0.0,'n':6}]
 run(path,check)
