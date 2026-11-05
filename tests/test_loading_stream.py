"""Integration checks for native Parquet/TSV decoding and validation."""
import asyncio
import csv
import json

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from backend.config import load_config
from backend.datasets import descriptor
from backend.graph_worker import GraphWorker
from tests.test_worker import fixture


def load_and_check(path, check=None, error=None):
    async def run():
        worker=GraphWorker(load_config())
        try:
            if error:
                with pytest.raises(Exception):
                    await worker.load(descriptor(str(path)))
                assert worker.state=='error' and not worker.ready
                assert error in worker.error
            else:
                await worker.load(descriptor(str(path)))
                if check: await check(worker)
        finally: await worker.stop()
    asyncio.run(run())


@pytest.mark.parametrize('implicit', [False, True])
def test_native_batches_and_scalar_properties(tmp_path, implicit):
    path=fixture(tmp_path)
    d=json.loads(path.read_text())
    n=32771
    # An ignored nested column precedes mapped scalars: Arrow top-level indexes
    # and Parquet leaf-column indexes differ and must not be confused.
    vertices=pa.table({'ignored':pa.array([{'x':[1,2]}]*n),
        'vid':pa.array(range(n),type=pa.int64()),
        'title':['中文节点' if i==0 else f'Node {i}' for i in range(n)],
        'flag':[i%2==0 for i in range(n)],
        'score':pa.array([None if i%3 else i/2 for i in range(n)],type=pa.float64()),
        'label':['属性' if i%2 else None for i in range(n)],
        'source':pa.array([-7]*n,type=pa.int64())})
    edges=pa.table({'ignored':pa.array([{'x':[1]}]*(n-1)),
        'u':pa.array(range(n-1),type=pa.int64()),'v':pa.array(range(1,n),type=pa.int64()),
        'w':pa.array([2]*(n-1),type=pa.int64()),'tag':['关系']*(n-1),
        'id':pa.array([-5]*(n-1),type=pa.int64())})
    pq.write_table(vertices,tmp_path/'nodes.parquet');pq.write_table(edges,tmp_path/'links.parquet')
    d['vertices']['properties']={'flag':'flag','score':'score','label':'label','source':'source'}
    d['edges']['columns']={'target':'v','source':'u',**({} if implicit else {'weight':'w'})}
    d['edges']['properties']={'tag':'tag','id':'id'}
    path.write_text(json.dumps(d),encoding='utf-8')
    async def check(worker):
        assert worker.info['vertices']==n and worker.info['edges']==n-1
        assert worker.info['total_weight']==(n-1)*(1 if implicit else 2)
        result=await worker.call('search_vertices',{'query':'中文节点'})
        assert result['rows'][0]['properties']=={'flag':True,'score':0.0,'label':None,'source':-7}
        stats=await worker.call('get_vertex_stats_unweighted',{'vertex_id':str(n-1)})
        assert stats['in_degree']==1 and stats['out_degree']==0
    load_and_check(path,check)


@pytest.mark.parametrize('field,value', [('u',None),('v',-1),('w',None),('w',-1),('score',float('nan')),('score',float('inf'))])
def test_native_validation_rejects_bad_values(tmp_path,field,value):
    path=fixture(tmp_path);d=json.loads(path.read_text())
    table=pq.read_table(tmp_path/'links.parquet').to_pydict()
    table['score']=[None,1.5,2.0];table[field][1]=value
    pq.write_table(pa.table(table),tmp_path/'links.parquet')
    d['edges']['properties']={'score':'score'};path.write_text(json.dumps(d))
    load_and_check(path,error=f'edges record 2, {field}:')


def test_quoted_tsv_unicode_paths_and_nullable_properties(tmp_path):
    folder=tmp_path/'带空格 中文';folder.mkdir()
    path=fixture(folder,tsv=True)
    d=json.loads(path.read_text());d['vertices']['properties']={'score':'score','flag':'flag'}
    d['vertices']['types'].update(score='float64',flag='bool')
    names=['含\t制表符','换\r\n行与"引号"','中文节点','']
    with (folder/'nodes.tsv').open('w',encoding='utf-8-sig',newline='') as f:
        writer=csv.writer(f,delimiter='\t');writer.writerow(['vid','title','score','flag'])
        for i,name in zip([10,20,614613821311819618,40],names):writer.writerow([i,name,' +1.25e1 ' if i==10 else '', 'true' if i==10 else ''])
    path.write_text(json.dumps(d),encoding='utf-8')
    async def check(worker):
        for vertex,name in zip([10,20,614613821311819618,40],names):
            result=await worker.call('search_vertices',{'query':str(vertex)})
            assert result['rows'][0]['name']==(name or str(vertex))
        result=await worker.call('search_vertices',{'query':'10'})
        assert result['rows'][0]['properties']=={'score':12.5,'flag':True}
    load_and_check(path,check)


@pytest.mark.parametrize('bad', ['01','-0','+1','9223372036854775808','true',''])
def test_tsv_structural_integer_lexical_rules(tmp_path,bad):
    path=fixture(tmp_path,tsv=True)
    (tmp_path/'links.tsv').write_text('u\tv\tw\n10\t20\t'+bad+'\n',encoding='utf-8')
    load_and_check(path,error='edges record 1, w:')


@pytest.mark.parametrize('mode', ['duplicate_id','wrong_type','missing_column','duplicate_header','truncated_quote','weight_overflow'])
def test_native_schema_and_structure_errors(tmp_path,mode):
    path=fixture(tmp_path,tsv=mode in ('duplicate_header','truncated_quote'))
    if mode=='duplicate_id':
        table=pq.read_table(tmp_path/'nodes.parquet').to_pydict();table['vid'][1]=10
        pq.write_table(pa.table(table),tmp_path/'nodes.parquet');error='duplicate external vertex ID'
    elif mode=='wrong_type':
        table=pq.read_table(tmp_path/'links.parquet').to_pydict();table['w']=pa.array([1,2,3],type=pa.int32())
        pq.write_table(pa.table(table),tmp_path/'links.parquet');error='unsupported type'
    elif mode=='missing_column':
        table=pq.read_table(tmp_path/'links.parquet').drop(['w']);pq.write_table(table,tmp_path/'links.parquet');error='missing Parquet column'
    elif mode=='duplicate_header':
        (tmp_path/'nodes.tsv').write_text('vid\ttitle\tvid\n',encoding='utf-8');error='duplicate TSV header'
    elif mode=='truncated_quote':
        (tmp_path/'links.tsv').write_text('u\tv\tw\n10\t20\t"1',encoding='utf-8');error='unterminated quoted TSV field'
    else:
        table=pq.read_table(tmp_path/'links.parquet').to_pydict();table['w']=[2**63-1,2,5]
        pq.write_table(pa.table(table),tmp_path/'links.parquet');error='weight overflow'
    load_and_check(path,error=error)


def test_empty_graph_and_int64_boundary(tmp_path):
    path=fixture(tmp_path,edges=[(10,2**63-1,2**63-1)])
    pq.write_table(pa.table({'vid':pa.array([10,2**63-1],type=pa.int64()),'title':pa.array([None,None],type=pa.string())}),tmp_path/'nodes.parquet')
    async def check(worker):
        assert worker.info['total_weight']==str(2**63-1)
        result=await worker.call('get_vertex_stats_weighted',{'vertex_id':str(2**63-1)})
        assert result['name']==str(2**63-1) and result['inflow']==str(2**63-1)
    load_and_check(path,check)
    pq.write_table(pa.table({'vid':pa.array([],type=pa.int64()),'title':pa.array([],type=pa.string())}),tmp_path/'nodes.parquet')
    pq.write_table(pa.table({k:pa.array([],type=pa.int64()) for k in ('u','v','w')}),tmp_path/'links.parquet')
    async def empty(worker):
        assert worker.info['vertices']==worker.info['edges']==worker.info['total_weight']==0
        assert (await worker.call('rank_vertices_weighted',{'metric':'total'}))['rows']==[]
    load_and_check(path,empty)


def test_invalid_utf8_in_ignored_tsv_field_is_rejected(tmp_path):
    path=fixture(tmp_path,tsv=True)
    (tmp_path/'links.tsv').write_bytes(b'u\tv\tw\tignored\n10\t20\t1\t\xff\n')
    load_and_check(path,error='invalid UTF-8 TSV field')
