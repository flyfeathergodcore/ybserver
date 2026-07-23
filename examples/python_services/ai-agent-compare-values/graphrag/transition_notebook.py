import pandas as pd
import json
import re

def merge_ports(port1, port2, port3):
    """合并数据接口、视频接口、音频接口"""
    ports = []
    
    for port in [port1, port2, port3]:
        if port is None or port == '':
            continue
        port_str = str(port).strip()
        if port_str:
            ports.append(port_str)
    
    return '，'.join(ports) if ports else ''

def transition_to_csv(file_path: str):
    with open(file_path, "r", encoding='utf-8') as f:
        transition_data = json.load(f)

    processed_data = []
    
    for item in transition_data:
        if not isinstance(item, dict):
            continue
        
        specs = item.get('specs')
        
        # 如果 specs 是 None 或空，创建空字典
        if specs is None or specs == {}:
            specs = {}
            item['specs'] = specs
        
        # ========== 从 specs 中获取接口数据 ==========
        port1 = specs.get('数据接口')      # 例如: "左侧：1×USB Type-A 5Gbps 右侧：2×全功能USB Type-C，1×USB Type-A 5Gbps"
        port2 = specs.get('视频接口')      # 例如: "1×HDMI2.1"
        port3 = specs.get('音频接口')      # 例如: "3.5mm耳机/麦克风二合一接口"
        
        # ========== 合并接口到 specs['数据接口'] ==========
        if port1 is not None or port2 is not None or port3 is not None:
            # 合并三个接口
            merged_ports = merge_ports(port1, port2, port3)
            
            # 更新 specs 中的数据接口
            specs['数据接口'] = merged_ports
            
            # 删除 specs 中的视频接口和音频接口
            specs.pop('视频接口', None)
            specs.pop('音频接口', None)
            
            print(f"合并接口: {port1} | {port2} | {port3} → {merged_ports}")
        else:
            # 如果没有接口数据，确保数据接口存在
            if '数据接口' not in specs:
                specs['数据接口'] = ''
        
        processed_data.append(item)
    
    # 转换
    if processed_data:
        df = pd.json_normalize(processed_data)
        
        # 重命名列：去掉 "specs." 前缀
        new_columns = []
        for col in df.columns:
            if col.startswith('specs.'):
                new_columns.append(col.replace('specs.', ''))
            else:
                new_columns.append(col)
        df.columns = new_columns
        
        # 删除可能残留的视频接口和音频接口列
        for col in ['视频接口', '音频接口']:
            if col in df.columns:
                df = df.drop(columns=[col])
        
        output_path = file_path.replace('.json', '_output.csv')
        df.to_csv(output_path, index=False, encoding='utf-8-sig')
        
        print(f"\n✅ 转换完成！输出文件: {output_path}")
        print(f"📊 共 {len(df)} 条记录，{len(df.columns)} 个字段")
        
        # 检查接口合并结果
        print("\n🔌 合并后的接口数据预览:")
        if '数据接口' in df.columns:
            for i, row in df.head(5).iterrows():
                name = row.get('name', '未知')
                ports = row.get('数据接口', '无')
                print(f"  {name}: {ports}")
        
        # 确认视频接口和音频接口已被移除
        print("\n📋 列名中包含'接口'的字段:")
        interface_cols = [col for col in df.columns if '接口' in col]
        if interface_cols:
            for col in interface_cols:
                print(f"  - {col}")
        else:
            print("  ✅ 没有包含'接口'的列（已全部合并到'数据接口'）")
        
    else:
        print("⚠️ 没有有效数据可转换")

def inspect_json_structure(file_path: str):
    """检查JSON数据结构"""
    with open(file_path, "r", encoding='utf-8') as f:
        data = json.load(f)
    
    print("=" * 60)
    print("📊 JSON 数据结构检查")
    print("=" * 60)
    
    if not data:
        print("❌ 数据为空")
        return
    
    print(f"📊 总记录数: {len(data)}")
    
    # 检查前2条数据
    for idx, item in enumerate(data[:2]):
        print(f"\n📋 第 {idx+1} 条数据的结构:")
        print(f"  顶层字段: {list(item.keys())}")
        
        specs = item.get('specs')
        if specs is not None and isinstance(specs, dict):
            print(f"  specs 字段: {list(specs.keys())}")
            for port_field in ['数据接口', '视频接口', '音频接口']:
                if port_field in specs:
                    print(f"  specs {port_field}: {specs[port_field]}")

def create_fulltext_index():
    """创建/更新全文索引，确保包含 Notebook 标签"""
    from neo4j import GraphDatabase

    uri = "bolt://localhost:7687"
    driver = GraphDatabase.driver(uri, auth=("neo4j", "yangboneo4j"))

    try:
        with driver.session() as session:
            # 先检查索引是否已存在
            result = session.run("SHOW INDEXES YIELD name WHERE name = 'entity_index' RETURN name").data()
            if result:
                print("📇 全文索引 'entity_index' 已存在，先删除再重建（确保包含 Notebook 标签）...")
                session.run("DROP INDEX entity_index")

            session.run("""
                CREATE FULLTEXT INDEX entity_index
                FOR (n:Phone|Notebook|Brand|CPU|GPU|OS|FEATURE)
                ON EACH [n.name, n.brand, n.series]
            """)
            print("✅ 全文索引 'entity_index' 创建成功（含 Phone/Notebook/Brand/CPU/GPU/OS/FEATURE）")
    except Exception as e:
        print(f"⚠️ 创建全文索引失败: {e}")
    finally:
        driver.close()

if __name__ == "__main__":
    file_path = "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/graphrag/dataset/notebooks_zol.json"

    # 先检查数据结构
    inspect_json_structure(file_path)

    print("\n" + "=" * 60)
    print("🔄 开始转换...")
    print("=" * 60)

    # 执行转换
    transition_to_csv(file_path)

    # 构建全文索引
    print("\n" + "=" * 60)
    print("📇 构建全文索引...")
    print("=" * 60)
    create_fulltext_index()