from neo4j import GraphDatabase
import pandas as pd
import re

# 连接Neo4j数据库
uri = "bolt://localhost:7687"
driver = GraphDatabase.driver(uri, auth=("neo4j", "yangboneo4j"))

def clear_db(tx):
    tx.run("MATCH (n) DETACH DELETE n")

def standardize_positioning(value):
    """标准化产品定位"""
    if pd.isna(value) or value == '':
        return None
    
    value = str(value)
    
    if '游戏' in value:
        return '游戏本'
    elif '创意' in value or '设计' in value:
        return '创意设计本'
    elif '二合一' in value or '翻转' in value:
        return '二合一笔记本'
    elif '轻薄' in value and '商务' in value:
        return '轻薄商务本'
    elif '轻薄' in value:
        return '轻薄本'
    elif '商务' in value or '商用' in value or '办公' in value:
        return '商务本'
    else:
        return '其他'

def extract_weight_kg(weight_value):
    """提取重量的数值（kg）"""
    if pd.isna(weight_value) or weight_value == '':
        return None
    
    try:
        if isinstance(weight_value, str):
            match = re.search(r'(\d+\.?\d*)', weight_value)
            if match:
                return float(match.group(1))
        else:
            return float(weight_value)
    except:
        return None
    return None

def classify_weight(weight_value):
    """将笔记本重量分类为等级"""
    weight_kg = extract_weight_kg(weight_value)
    
    if weight_kg is None:
        return '未知'
    
    if weight_kg < 1.0:
        return '超轻 (<1kg)'
    elif weight_kg < 1.3:
        return '轻薄 (1-1.3kg)'
    elif weight_kg < 1.6:
        return '主流 (1.3-1.6kg)'
    elif weight_kg < 2.0:
        return '中等 (1.6-2.0kg)'
    elif weight_kg < 2.5:
        return '较重 (2.0-2.5kg)'
    elif weight_kg < 3.0:
        return '重型 (2.5-3.0kg)'
    else:
        return '超重 (>3kg)'

def classify_screen_size(size_value):
    """将屏幕尺寸分类"""
    if pd.isna(size_value) or size_value == '':
        return '未知'
    
    try:
        if isinstance(size_value, str):
            match = re.search(r'(\d+\.?\d*)', size_value)
            if match:
                size = float(match.group(1))
            else:
                return '未知'
        else:
            size = float(size_value)
    except:
        return '未知'
    
    if size < 13.0:
        return '小屏 (<13英寸)'
    elif size < 14.5:
        return '中屏 (13-14.5英寸)'
    elif size < 16.0:
        return '大屏 (14.5-16英寸)'
    else:
        return '超大屏 (>16英寸)'

def extract_release_year(release_date):
    """从上市时间中提取年份"""
    if pd.isna(release_date) or release_date == '':
        return None
    
    date_str = str(release_date)
    match = re.search(r'(20\d{2})', date_str)
    if match:
        return int(match.group(1))
    return None

def count_ports(ports_str):
    """统计数据接口总数量 - 忽略左右侧位置"""
    if pd.isna(ports_str) or ports_str == '':
        return 0
    
    ports_text = str(ports_str)
    
    # 移除左右侧等位置描述
    ports_text = re.sub(r'左侧[：:]\s*', '', ports_text)
    ports_text = re.sub(r'右侧[：:]\s*', '', ports_text)
    ports_text = re.sub(r'后置[：:]\s*', '', ports_text)
    
    # 统计所有 "×" 前的数字之和
    numbers = re.findall(r'(\d+)\s*[×xX]', ports_text)
    if numbers:
        return sum(int(n) for n in numbers)
    
    return 0

def classify_port_count(port_count):
    """将接口数量分类"""
    if port_count is None or port_count == 0:
        return '无接口'
    elif port_count <= 2:
        return '接口少 (≤2个)'
    elif port_count <= 4:
        return '接口适中 (3-4个)'
    elif port_count <= 6:
        return '接口丰富 (5-6个)'
    else:
        return '接口丰富 (>6个)'

def create_laptop_graph(tx, row, index):
    # 1. 品牌节点
    if pd.notna(row['brand']) and row['brand'] != '':
        tx.run("MERGE (b:Brand {name: $brand})", brand=row['brand'])
    
    # 2. CPU节点
    cpu_name = row['CPU型号']
    if pd.notna(cpu_name) and cpu_name != '':
        tx.run("""
            MERGE (c:CPU {name: $name})
            SET c.frequency = $freq
        """, name=cpu_name, freq=row.get('CPU主频', ''))
    
    # 3. GPU节点
    gpu_name = row['显卡芯片']
    if pd.notna(gpu_name) and gpu_name != '':
        tx.run("MERGE (g:GPU {name: $name})", name=gpu_name)
    
    # 4. 屏幕分辨率节点
    screen = row['屏幕分辨率']
    if pd.notna(screen) and screen != '':
        tx.run("MERGE (s:SCREEN {resolution: $resolution})", resolution=screen)
    
    # 5. 屏幕刷新率节点
    refresh_rate = row['屏幕刷新率']
    if pd.notna(refresh_rate) and refresh_rate != '':
        tx.run("MERGE (r:REFRESH_RATE {rate: $rate})", rate=refresh_rate)
    
    # 6. 产品定位节点 - 标准化
    positioning_raw = row['产品定位']
    positioning = standardize_positioning(positioning_raw)
    if positioning:
        tx.run("MERGE (p:POSITIONING {position: $position})", position=positioning)
    
    # 7. 重量分类节点 - 使用 category 代替 class
    weight_raw = row['笔记本重量']
    weight_class = classify_weight(weight_raw)
    weight_kg = extract_weight_kg(weight_raw)
    if weight_class:
        tx.run("MERGE (w:WEIGHT_CLASS {category: $weight_class})", weight_class=weight_class)
    
    # 8. 屏幕尺寸分类节点 - 使用 category 代替 class
    screen_size_raw = row['屏幕尺寸']
    screen_class = classify_screen_size(screen_size_raw)
    if screen_class:
        tx.run("MERGE (s:SCREEN_SIZE_CLASS {category: $screen_class})", screen_class=screen_class)
    
    # 9. 上市年份节点
    release_date_raw = row['上市时间']
    release_year = extract_release_year(release_date_raw)
    if release_year:
        tx.run("MERGE (r:RELEASE_YEAR {year: $year})", year=release_year)
    
    # 10. 人脸识别节点
    face_recognition = row['人脸识别']
    if pd.notna(face_recognition) and face_recognition != '':
        tx.run("MERGE (f:FACE_RECOGNITION {feature: $feature})", feature=face_recognition)
    
    # 11. 指纹识别节点
    fingerprint_recognition = row['指纹识别']
    if pd.notna(fingerprint_recognition) and fingerprint_recognition != '':
        tx.run("MERGE (f:FINGERPRINT_RECOGNITION {feature: $feature})", feature=fingerprint_recognition)
    
    # 12. 数据接口节点 - 使用 category 代替 class
    ports_raw = row['数据接口']
    port_count = count_ports(ports_raw)
    port_class = classify_port_count(port_count)
    if port_count > 0:
        tx.run("MERGE (p:PORT_COUNT {count: $count, category: $port_class})", 
               count=port_count, port_class=port_class)
    
    # 13. 创建笔记本主节点
    zol_id = str(row['zol_id']) if pd.notna(row['zol_id']) else f"temp_{index}"
    laptop_props = {
        'zol_id': zol_id,
        'name': str(row['name']) if pd.notna(row['name']) else '',
        'jd_price': str(row['jd_price']) if pd.notna(row['jd_price']) else '',
        'weight_raw': str(weight_raw) if pd.notna(weight_raw) else '',
        'weight_kg': weight_kg,
        'weight_class': weight_class,
        'screen_size_raw': str(screen_size_raw) if pd.notna(screen_size_raw) else '',
        'screen_class': screen_class,
        'resolution': str(row['屏幕分辨率']) if pd.notna(row['屏幕分辨率']) else '',
        'refresh_rate': str(row['屏幕刷新率']) if pd.notna(row['屏幕刷新率']) else '',
        'fingerprint_recognition': str(row['指纹识别']) if pd.notna(row['指纹识别']) else '',
        'positioning_raw': str(positioning_raw) if pd.notna(positioning_raw) else '',
        'positioning': positioning if positioning else '未分类',
        'release_year': release_year,
        'face_recognition': str(row['人脸识别']) if pd.notna(row['人脸识别']) else '',
        'ports_raw': str(ports_raw) if pd.notna(ports_raw) else '',
        'port_count': port_count,
        'port_class': port_class
    }
    
    tx.run("""
        MERGE (l:Laptop {zol_id: $zol_id})
        SET l.name = $name,
            l.jd_price = $jd_price,
            l.weight_raw = $weight_raw,
            l.weight_kg = $weight_kg,
            l.weight_class = $weight_class,
            l.screen_size_raw = $screen_size_raw,
            l.screen_class = $screen_class,
            l.resolution = $resolution,
            l.refresh_rate = $refresh_rate,
            l.fingerprint_recognition = $fingerprint_recognition,
            l.positioning_raw = $positioning_raw,
            l.positioning = $positioning,
            l.release_year = $release_year,
            l.face_recognition = $face_recognition,
            l.ports_raw = $ports_raw,
            l.port_count = $port_count,
            l.port_class = $port_class
    """, **laptop_props)
    
    # 14. 建立关系
    # 笔记本 -> 品牌
    if pd.notna(row['brand']) and row['brand'] != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (b:Brand {name: $brand})
            MERGE (l)-[:BELONGS_TO]->(b)
        """, zol_id=zol_id, brand=row['brand'])
    
    # 笔记本 -> CPU
    if pd.notna(cpu_name) and cpu_name != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (c:CPU {name: $cpu})
            MERGE (l)-[:HAS_CPU]->(c)
        """, zol_id=zol_id, cpu=cpu_name)
    
    # 笔记本 -> GPU
    if pd.notna(gpu_name) and gpu_name != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (g:GPU {name: $gpu})
            MERGE (l)-[:HAS_GPU]->(g)
        """, zol_id=zol_id, gpu=gpu_name)
    
    # 笔记本 -> 屏幕分辨率
    if pd.notna(screen) and screen != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (s:SCREEN {resolution: $screen})
            MERGE (l)-[:HAS_SCREEN]->(s)
        """, zol_id=zol_id, screen=screen)
    
    # 笔记本 -> 刷新率
    if pd.notna(refresh_rate) and refresh_rate != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (r:REFRESH_RATE {rate: $refresh_rate})
            MERGE (l)-[:HAS_REFRESH_RATE]->(r)
        """, zol_id=zol_id, refresh_rate=refresh_rate)
    
    # 笔记本 -> 产品定位
    if positioning:
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (p:POSITIONING {position: $positioning})
            MERGE (l)-[:HAS_POSITIONING]->(p)
        """, zol_id=zol_id, positioning=positioning)
    
    # 笔记本 -> 重量分类
    if weight_class:
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (w:WEIGHT_CLASS {category: $weight_class})
            MERGE (l)-[:HAS_WEIGHT_CLASS]->(w)
        """, zol_id=zol_id, weight_class=weight_class)
    
    # 笔记本 -> 屏幕尺寸分类
    if screen_class:
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (s:SCREEN_SIZE_CLASS {category: $screen_class})
            MERGE (l)-[:HAS_SCREEN_SIZE_CLASS]->(s)
        """, zol_id=zol_id, screen_class=screen_class)
    
    # 笔记本 -> 上市年份
    if release_year:
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (r:RELEASE_YEAR {year: $release_year})
            MERGE (l)-[:RELEASED_IN]->(r)
        """, zol_id=zol_id, release_year=release_year)
    
    # 笔记本 -> 人脸识别
    if pd.notna(face_recognition) and face_recognition != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (f:FACE_RECOGNITION {feature: $face_recognition})
            MERGE (l)-[:HAS_FACE_RECOGNITION]->(f)
        """, zol_id=zol_id, face_recognition=face_recognition)
    
    # 笔记本 -> 指纹识别
    if pd.notna(fingerprint_recognition) and fingerprint_recognition != '':
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (f:FINGERPRINT_RECOGNITION {feature: $fingerprint_recognition})
            MERGE (l)-[:HAS_FINGERPRINT_RECOGNITION]->(f)
        """, zol_id=zol_id, fingerprint_recognition=fingerprint_recognition)
    
    # 笔记本 -> 接口数量
    if port_count > 0:
        tx.run("""
            MATCH (l:Laptop {zol_id: $zol_id})
            MATCH (p:PORT_COUNT {count: $port_count})
            MERGE (l)-[:HAS_PORT_COUNT]->(p)
        """, zol_id=zol_id, port_count=port_count)

def print_stats(df):
    """打印数据统计"""
    print("\n" + "="*60)
    print("📊 数据统计预览")
    print("="*60)
    
    # 重量分布
    print("\n📊 重量分类分布:")
    df['weight_class'] = df['笔记本重量'].apply(classify_weight)
    weight_counts = df['weight_class'].value_counts()
    for cls, count in weight_counts.items():
        print(f"  {cls}: {count}台")
    
    # 产品定位分布
    print("\n📊 产品定位分类分布:")
    df['positioning_std'] = df['产品定位'].apply(standardize_positioning)
    pos_counts = df['positioning_std'].value_counts()
    for pos, count in pos_counts.items():
        print(f"  {pos}: {count}台")
    
    # 屏幕尺寸分布
    print("\n📊 屏幕尺寸分类分布:")
    df['screen_class'] = df['屏幕尺寸'].apply(classify_screen_size)
    screen_counts = df['screen_class'].value_counts()
    for cls, count in screen_counts.items():
        print(f"  {cls}: {count}台")
    
    # 上市年份分布
    print("\n📊 上市年份分布:")
    df['release_year'] = df['上市时间'].apply(extract_release_year)
    year_counts = df['release_year'].value_counts().sort_index()
    for year, count in year_counts.items():
        print(f"  {year}年: {count}台")
    
    # 接口数量分布
    print("\n📊 接口数量分布:")
    df['port_count'] = df['数据接口'].apply(count_ports)
    port_counts = df['port_count'].value_counts().sort_index()
    for count, num in port_counts.items():
        print(f"  {count}个接口: {num}台")

def main():
    try:
        print("正在读取CSV文件...")
        df = pd.read_csv('/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/graphrag/dataset/notebooks_zol_output.csv', encoding='utf-8-sig')
        print(f"✅ 读取到 {len(df)} 条记录")
        
        # 打印列名帮助调试
        print("\n📋 CSV列名（前20个）:")
        for i, col in enumerate(df.columns[:20]):
            print(f"  {i+1}. {col}")
        
        # 打印统计信息
        print_stats(df)
        
        with driver.session() as session:
            result = session.run("RETURN 1 AS test")
            print("\n✅ Neo4j连接成功！")
            
            # 清空数据库
            session.execute_write(clear_db)
            print("数据库已清空")
            
            print("\n开始导入数据...")
            success_count = 0
            total = len(df)
            
            for index, row in df.iterrows():
                try:
                    session.execute_write(create_laptop_graph, row, index)
                    success_count += 1
                except Exception as e:
                    print(f"❌ 处理第 {index+1} 行时出错: {e}")
                    print(f"   笔记本名称: {row.get('name', '未知')}")
                
                if (index + 1) % 10 == 0:
                    print(f"进度: {index + 1}/{total} (成功: {success_count})")
            
            print(f"\n✅ 导入完成！成功导入 {success_count}/{total} 条记录")
            
    except Exception as e:
        print(f"❌ 错误: {e}")
        import traceback
        traceback.print_exc()
    finally:
        driver.close()

if __name__ == "__main__":
    main()