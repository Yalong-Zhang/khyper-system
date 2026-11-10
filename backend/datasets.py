"""Descriptor/path preflight only; C++ owns source-file schemas and record validation."""
from __future__ import annotations
import json
from pathlib import Path


def descriptor(path: str) -> dict:
    file = Path(path).expanduser().resolve(strict=True)
    if not file.is_file() or file.stat().st_size > 1_000_000:
        raise ValueError("请选择有效的 dataset.json 描述文件。")
    d = json.loads(file.read_text(encoding="utf-8-sig"))
    required = {"format", "id", "name", "directed", "vertices", "edges"}
    if not isinstance(d, dict) or not required <= d.keys() or d.keys()-required-{"description"}:
        raise ValueError("描述文件字段缺失或包含未知字段。")
    if d["format"] != "khyper.graph.v1" or type(d["directed"]) is not bool:
        raise ValueError("格式必须为 khyper.graph.v1，并显式声明 directed 布尔值。")
    for key in ("id", "name"):
        if not isinstance(d[key], str) or not d[key].strip():
            raise ValueError(f"{key} 必须为非空字符串。")
    if "description" in d and not isinstance(d["description"], str):
        raise ValueError("description 必须为字符串。")
    for kind in ("vertices", "edges"):
        t=d[kind]
        allowed={"path","format","columns","properties","types","quoting"}
        if kind=="edges":allowed|={"weight_semantics","weight_unit"}
        if not isinstance(t,dict) or not {"path","format","columns"}<=t.keys() or t.keys()-allowed:
            raise ValueError(f"{kind}: 无效表定义。")
        if t["format"] not in ("parquet","tsv") or not isinstance(t["path"],str):
            raise ValueError(f"{kind}: 仅支持本地 Parquet / TSV 文件。")
        if 'quoting' in t and (t['format']!='tsv' or t['quoting'] not in ('csv','none')):
            raise ValueError(f'{kind}: quoting 仅适用于 TSV，必须为 csv 或 none。')
        p=(file.parent/t["path"]).resolve(strict=True)
        if not p.is_file():raise ValueError(f"{kind}: 路径不是文件。")
        cols=t["columns"];props=t.get("properties",{})
        roles={"id","name","type"} if kind=="vertices" else {"source","target","weight","type"}
        needed={"id"} if kind=="vertices" else {"source","target"}
        if not isinstance(cols,dict) or not needed<=cols.keys() or cols.keys()-roles or not isinstance(props,dict):
            raise ValueError(f"{kind}: 无效结构字段映射。")
        if roles & props.keys():raise ValueError(f"{kind}: 属性名与结构角色冲突。")
        names=list(cols.values())+list(props.values())
        if any(not isinstance(x,str) or not x for x in [*props.keys(),*names]) or len(set(names))!=len(names):
            raise ValueError(f"{kind}: 字段名称为空或重复映射。")
        if kind=="edges" and (not isinstance(t.get("weight_semantics"),str) or not t["weight_semantics"].strip()):
            raise ValueError("edges.weight_semantics 必须说明权重含义。")
        if "weight_unit" in t and (not isinstance(t['weight_unit'],str) or not t['weight_unit'].strip()):
            raise ValueError("weight_unit 必须为非空字符串。")
        t['_path']=p
    d['_descriptor']=file
    return d

