from neo4j import GraphDatabase
import pandas as pd
import re

# 连接Neo4j数据库
uri = "bolt://localhost:7687"
driver = GraphDatabase.driver(uri, auth=("neo4j", "yangboneo4j"))

def clear_db(tx):
    tx.run("MATCH (n) DETACH DELETE n")

def extract_battery_capacity(battery_str):
    """提取电池容量（mAh）"""
    if pd.isna(battery_str) or battery_str == '':
        return None
    match = re.search(r'(\d+)mAh', str(battery_str))
    if match:
        return int(match.group(1))
    return None

def classify_battery(battery_mah):
    """电池容量分类"""
    if battery_mah is None:
        return '未知'
    if battery_mah < 4000:
        return '小电池 (<4000mAh)'
    elif battery_mah < 5000:
        return '标准电池 (4000-5000mAh)'
    elif battery_mah < 6000:
        return '大电池 (5000-6000mAh)'
    elif battery_mah < 8000:
        return '超大电池 (6000-8000mAh)'
    else:
        return '巨无霸电池 (>8000mAh)'

def classify_price(price_str):
    """手机价格分类"""
    if pd.isna(price_str) or price_str == '':
        return '未知'
    try:
        price = float(re.sub(r'[¥,\s]', '', str(price_str)))
        if price < 2000:
            return '入门 (<2000)'
        elif price < 4000:
            return '中端 (2000-4000)'
        elif price < 6000:
            return '高端 (4000-6000)'
        elif price < 8000:
            return '旗舰 (6000-8000)'
        else:
            return '超旗舰 (>8000)'
    except:
        return '未知'

def extract_charging_watt(charging_str):
    """提取充电功率"""
    if pd.isna(charging_str) or charging_str == '':
        return None
    match = re.search(r'(\d+)w', str(charging_str).lower())
    if match:
        return int(match.group(1))
    return None

def extract_screen_size(size_str):
    """提取屏幕尺寸数值"""
    if pd.isna(size_str) or size_str == '':
        return None
    match = re.search(r'(\d+\.?\d*)英寸', str(size_str))
    if match:
        return float(match.group(1))
    return None

def classify_screen_size(size_value):
    """屏幕尺寸分类"""
    if size_value is None:
        return '未知'
    if size_value < 6.0:
        return '小屏 (<6英寸)'
    elif size_value < 6.5:
        return '中屏 (6-6.5英寸)'
    elif size_value < 6.8:
        return '大屏 (6.5-6.8英寸)'
    else:
        return '超大屏 (>6.8英寸)'

def extract_weight_grams(weight_str):
    """提取重量（g）"""
    if pd.isna(weight_str) or weight_str == '':
        return None
    match = re.search(r'(\d+)g', str(weight_str))
    if match:
        return int(match.group(1))
    return None

def classify_weight(weight_g):
    """重量分类"""
    if weight_g is None:
        return '未知'
    if weight_g < 180:
        return '轻量 (<180g)'
    elif weight_g < 200:
        return '适中 (180-200g)'
    elif weight_g < 220:
        return '较重 (200-220g)'
    else:
        return '重量级 (>220g)'

def has_5g(network_str):
    """检查是否支持5G"""
    if pd.isna(network_str) or network_str == '':
        return False
    return '5G' in str(network_str)

def has_nfc(nfc_str):
    """检查是否支持NFC"""
    if pd.isna(nfc_str) or nfc_str == '':
        return False
    return '支持' in str(nfc_str)

def has_face_unlock(face_str):
    """检查是否支持人脸识别"""
    if pd.isna(face_str) or face_str == '':
        return False
    return '支持' in str(face_str)

def create_phone_graph(tx, row, index):
    """创建手机节点和关系"""
    
    # 1. 品牌节点（复用笔记本的品牌节点）
    if pd.notna(row['brand']) and row['brand'] != '':
        tx.run("MERGE (b:Brand {name: $brand})", brand=row['brand'])
    
    # 2. CPU节点
    cpu_name = row['CPU型号']
    if pd.notna(cpu_name) and cpu_name != '':
        tx.run("""
            MERGE (c:CPU {name: $name})
            SET c.frequency = $freq,
                c.cores = $cores
        """, 
            name=cpu_name,
            freq=row.get('CPU频率', ''),
            cores=row.get('CPU核心数', '')
        )
    
    # 3. GPU节点
    gpu_name = row['GPU型号']
    if pd.notna(gpu_name) and gpu_name != '':
        tx.run("MERGE (g:GPU {name: $name})", name=gpu_name)
    
    # 4. 操作系统节点
    os_name = row['操作系统']
    if pd.notna(os_name) and os_name != '':
        tx.run("MERGE (os:OS {name: $name})", name=os_name)
    
    # 5. 价格分类节点
    price_class = row.get('price_class', '未知')
    if price_class:
        tx.run("MERGE (p:PRICE_CLASS {category: $category})", category=price_class)
    
    # 6. 电池分类节点
    battery_class = row.get('battery_class', '未知')
    if battery_class:
        tx.run("MERGE (b:BATTERY_CLASS {category: $category})", category=battery_class)
    
    # 7. 屏幕尺寸分类节点
    screen_class = row.get('screen_class', '未知')
    if screen_class:
        tx.run("MERGE (s:SCREEN_CLASS {category: $category})", category=screen_class)
    
    # 8. 重量分类节点
    weight_class = row.get('weight_class', '未知')
    if weight_class:
        tx.run("MERGE (w:WEIGHT_CLASS {category: $category})", category=weight_class)
    
    # 9. 5G支持节点
    if has_5g(row.get('网络类型', '')):
        tx.run("MERGE (g:FEATURE {type: '5G'})")
    
    # 10. NFC支持节点
    if has_nfc(row.get('NFC', '')):
        tx.run("MERGE (f:FEATURE {type: 'NFC'})")
    
    # 11. 人脸识别支持节点
    if has_face_unlock(row.get('面部识别', '')):
        tx.run("MERGE (f:FEATURE {type: 'Face Unlock'})")
    
    # 12. 创建手机主节点
    phone_id = str(row['zol_id']) if pd.notna(row['zol_id']) and str(row['zol_id']).strip() != '' else f"phone_{index}"
    
    # 提取品牌
    brand_name = row['brand'] if pd.notna(row['brand']) else ''
    
    phone_props = {
        'phone_id': phone_id,
        'name': str(row['name']) if pd.notna(row['name']) else '',
        'brand': brand_name,
        'series': str(row['series']) if pd.notna(row['series']) else '',
        'price_ref': str(row['price_ref']) if pd.notna(row['price_ref']) else '',
        'jd_price': str(row['jd_price']) if pd.notna(row['jd_price']) else '',
        'price_class': price_class,
        'release_date': str(row['国内发布时间']) if pd.notna(row['国内发布时间']) else '',
        'screen_size': str(row['屏幕尺寸']) if pd.notna(row['屏幕尺寸']) else '',
        'screen_class': screen_class,
        'resolution': str(row['分辨率']) if pd.notna(row['分辨率']) else '',
        'refresh_rate': str(row['屏幕刷新率']) if pd.notna(row['屏幕刷新率']) else '',
        'battery_capacity': str(row['电池容量']) if pd.notna(row['电池容量']) else '',
        'battery_mah': row.get('battery_mah'),
        'battery_class': battery_class,
        'charging_watt': row.get('charging_watt'),
        'ram': str(row['RAM容量']) if pd.notna(row['RAM容量']) else '',
        'storage': str(row['ROM容量']) if pd.notna(row['ROM容量']) else '',
        'weight': str(row['重量']) if pd.notna(row['重量']) else '',
        'weight_class': weight_class,
        'os': str(os_name) if pd.notna(os_name) else '',
        'fingerprint': str(row['指纹识别']) if pd.notna(row['指纹识别']) else '',
        'face_unlock': str(row['面部识别']) if pd.notna(row['面部识别']) else '',
        'nfc': str(row['NFC']) if pd.notna(row['NFC']) else '',
        'waterproof': str(row['三防功能']) if pd.notna(row['三防功能']) else '',
        'camera_pixels': str(row['像素']) if pd.notna(row['像素']) else '',
        'has_5g': has_5g(row.get('网络类型', ''))
    }
    
    tx.run("""
        MERGE (p:Phone {phone_id: $phone_id})
        SET p.name = $name,
            p.brand = $brand,
            p.series = $series,
            p.price_ref = $price_ref,
            p.jd_price = $jd_price,
            p.price_class = $price_class,
            p.release_date = $release_date,
            p.screen_size = $screen_size,
            p.screen_class = $screen_class,
            p.resolution = $resolution,
            p.refresh_rate = $refresh_rate,
            p.battery_capacity = $battery_capacity,
            p.battery_mah = $battery_mah,
            p.battery_class = $battery_class,
            p.charging_watt = $charging_watt,
            p.ram = $ram,
            p.storage = $storage,
            p.weight = $weight,
            p.weight_class = $weight_class,
            p.os = $os,
            p.fingerprint = $fingerprint,
            p.face_unlock = $face_unlock,
            p.nfc = $nfc,
            p.waterproof = $waterproof,
            p.camera_pixels = $camera_pixels,
            p.has_5g = $has_5g
    """, **phone_props)
    
    # 13. 建立关系
    # 手机 -> 品牌
    if brand_name:
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (b:Brand {name: $brand})
            MERGE (p)-[:BELONGS_TO]->(b)
        """, phone_id=phone_id, brand=brand_name)
    
    # 手机 -> CPU
    if pd.notna(cpu_name) and cpu_name != '':
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (c:CPU {name: $cpu})
            MERGE (p)-[:HAS_CPU]->(c)
        """, phone_id=phone_id, cpu=cpu_name)
    
    # 手机 -> GPU
    if pd.notna(gpu_name) and gpu_name != '':
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (g:GPU {name: $gpu})
            MERGE (p)-[:HAS_GPU]->(g)
        """, phone_id=phone_id, gpu=gpu_name)
    
    # 手机 -> 操作系统
    if pd.notna(os_name) and os_name != '':
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (os:OS {name: $os})
            MERGE (p)-[:RUNS_ON]->(os)
        """, phone_id=phone_id, os=os_name)
    
    # 手机 -> 价格分类
    if price_class:
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (pc:PRICE_CLASS {category: $price_class})
            MERGE (p)-[:HAS_PRICE_CLASS]->(pc)
        """, phone_id=phone_id, price_class=price_class)
    
    # 手机 -> 电池分类
    if battery_class:
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (bc:BATTERY_CLASS {category: $battery_class})
            MERGE (p)-[:HAS_BATTERY_CLASS]->(bc)
        """, phone_id=phone_id, battery_class=battery_class)
    
    # 手机 -> 屏幕分类 
    if screen_class:
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (sc:SCREEN_CLASS {category: $screen_class})
            MERGE (p)-[:HAS_SCREEN_CLASS]->(sc)
        """, phone_id=phone_id, screen_class=screen_class)
    
    # 手机 -> 重量分类
    if weight_class:
        tx.run("""
            MATCH (p:Phone {phone_id: $phone_id})
            MATCH (wc:WEIGHT_CLASS {category: $weight_class})
            MERGE (p)-[:HAS_WEIGHT_CLASS]->(wc)
        """, phone_id=phone_id, weight_class=weight_class)

def print_stats(df):
    """打印数据统计"""
    print("\n" + "="*60)
    print("📊 手机数据统计预览")
    print("="*60)
    
    print("\n📊 品牌分布:")
    brand_counts = df['brand'].value_counts()
    for brand, count in brand_counts.items():
        print(f"  {brand}: {count}台")
    
    print("\n📊 价格分类分布:")
    price_counts = df['price_class'].value_counts()
    for cls, count in price_counts.items():
        print(f"  {cls}: {count}台")
    
    print("\n📊 电池分类分布:")
    battery_counts = df['battery_class'].value_counts()
    for cls, count in battery_counts.items():
        print(f"  {cls}: {count}台")
    
    print("\n📊 屏幕尺寸分类分布:")
    screen_counts = df['screen_class'].value_counts()
    for cls, count in screen_counts.items():
        print(f"  {cls}: {count}台")
    
    print("\n📊 5G支持:")
    df['has_5g'] = df['网络类型'].apply(has_5g)
    print(f"  支持5G: {df['has_5g'].sum()}台")
    print(f"  不支持5G: {(~df['has_5g']).sum()}台")

def create_fulltext_index(session):
    """创建全文索引，用于加速名称/品牌搜索"""
    try:
        # 先检查索引是否已存在
        result = session.run("SHOW INDEXES YIELD name WHERE name = 'entity_index' RETURN name").data()
        if result:
            print("📇 全文索引 'entity_index' 已存在，跳过创建")
            return

        session.run("""
            CREATE FULLTEXT INDEX entity_index
            FOR (n:Phone|Notebook|Brand|CPU|GPU|OS|FEATURE)
            ON EACH [n.name, n.brand, n.series]
        """)
        print("✅ 全文索引 'entity_index' 创建成功")
    except Exception as e:
        print(f"⚠️ 创建全文索引失败: {e}")

def main():
    try:
        print("正在读取手机CSV文件...")
        df = pd.read_csv('/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/graphrag/dataset/phones_zol_output.csv', encoding='utf-8-sig')
        print(f"✅ 读取到 {len(df)} 条记录")
        
        # 添加计算字段
        df['battery_mah'] = df['电池容量'].apply(extract_battery_capacity)
        df['battery_class'] = df['battery_mah'].apply(classify_battery)
        df['price_class'] = df['jd_price'].apply(classify_price)
        df['charging_watt'] = df['有线充电'].apply(extract_charging_watt)
        df['screen_size_num'] = df['屏幕尺寸'].apply(extract_screen_size)
        df['screen_class'] = df['screen_size_num'].apply(classify_screen_size)
        df['weight_g'] = df['重量'].apply(extract_weight_grams)
        df['weight_class'] = df['weight_g'].apply(classify_weight)
        
        # 打印统计信息
        print_stats(df)
        
        with driver.session() as session:
            result = session.run("RETURN 1 AS test")
            print("\n✅ Neo4j连接成功！")
            
            print("\n开始导入手机数据...")
            success_count = 0
            total = len(df)
            
            for index, row in df.iterrows():
                try:
                    session.execute_write(create_phone_graph, row, index)
                    success_count += 1
                except Exception as e:
                    print(f"❌ 处理第 {index+1} 行时出错: {e}")
                    print(f"   手机名称: {row.get('name', '未知')}")
                
                if (index + 1) % 5 == 0:
                    print(f"进度: {index + 1}/{total} (成功: {success_count})")
            
            print(f"\n✅ 导入完成！成功导入 {success_count}/{total} 条记录")

            # 创建全文索引，加速名称搜索
            print("\n📇 创建全文索引...")
            create_fulltext_index(session)
    except Exception as e:
        print(f"❌ 错误: {e}")
        import traceback
        traceback.print_exc()
    finally:
        driver.close()

if __name__ == "__main__":
    main()