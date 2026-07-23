import pandas as pd
import json
import re

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

def transition_to_csv(file_path: str):
    with open(file_path, "r", encoding='utf-8') as f:
        data = json.load(f)

    processed_data = []
    
    for item in data:
        if not isinstance(item, dict):
            continue
        
        specs = item.get('specs', {})
        
        # 提取关键字段
        row = {
            'name': item.get('name', ''),
            'brand': item.get('brand', ''),
            'series': item.get('series', ''),
            'price_ref': item.get('price_ref', ''),
            'jd_price': item.get('jd_price', ''),
            'info': item.get('info', ''),
            'zol_id': item.get('zol_id', ''),
            
            # 从specs提取
            '国内发布时间': specs.get('国内发布时间', ''),
            '上市日期': specs.get('上市日期', ''),
            '产品型号': specs.get('产品型号', ''),
            '使用场景': specs.get('使用场景', ''),
            '机身材质': specs.get('机身材质', ''),
            '机身颜色': specs.get('机身颜色', ''),
            '指纹识别': specs.get('指纹识别', ''),
            '面部识别': specs.get('面部识别', ''),
            '长度': specs.get('长度', ''),
            '宽度': specs.get('宽度', ''),
            '厚度': specs.get('厚度', ''),
            '重量': specs.get('重量', ''),
            'CPU型号': specs.get('CPU型号', ''),
            'CPU频率': specs.get('CPU频率', ''),
            'CPU核心数': specs.get('CPU核心数', ''),
            'GPU型号': specs.get('GPU型号', ''),
            'RAM容量': specs.get('RAM容量', ''),
            'RAM存储类型': specs.get('RAM存储类型', ''),
            'ROM容量': specs.get('ROM容量', ''),
            'ROM存储类型': specs.get('ROM存储类型', ''),
            '操作系统': specs.get('操作系统', ''),
            '出厂系统内核': specs.get('出厂系统内核', ''),
            '扬声器': specs.get('扬声器', ''),
            '振动马达': specs.get('振动马达', ''),
            '散热': specs.get('散热', ''),
            '屏幕尺寸': specs.get('屏幕尺寸', ''),
            '屏幕类型': specs.get('屏幕类型', ''),
            '分辨率': specs.get('分辨率', ''),
            '屏幕材质': specs.get('屏幕材质', ''),
            '屏幕刷新率': specs.get('屏幕刷新率', ''),
            '像素密度': specs.get('像素密度', ''),
            '屏占比': specs.get('屏占比', ''),
            '触控采样率': specs.get('触控采样率', ''),
            '屏幕色彩': specs.get('屏幕色彩', ''),
            'HDR技术': specs.get('HDR技术', ''),
            '屏幕亮度': specs.get('屏幕亮度', ''),
            '屏幕盖板': specs.get('屏幕盖板', ''),
            '摄像头总数': specs.get('摄像头总数', ''),
            '摄像头名称': specs.get('摄像头名称', ''),
            '像素': specs.get('像素', ''),
            '光圈': specs.get('光圈', ''),
            '传感器型号': specs.get('传感器型号', ''),
            '传感器尺寸': specs.get('传感器尺寸', ''),
            '对焦方式': specs.get('对焦方式', ''),
            '防抖功能': specs.get('防抖功能', ''),
            '后置拍照功能': specs.get('后置拍照功能', ''),
            '后置视频拍摄': specs.get('后置视频拍摄', ''),
            '前置拍照功能': specs.get('前置拍照功能', ''),
            '前置视频拍摄': specs.get('前置视频拍摄', ''),
            '联名品牌': specs.get('联名品牌', ''),
            '闪光灯': specs.get('闪光灯', ''),
            '变焦倍数': specs.get('变焦倍数', ''),
            '网络类型': specs.get('网络类型', ''),
            '5G网络': specs.get('5G网络', ''),
            '4G网络': specs.get('4G网络', ''),
            '3G网络': specs.get('3G网络', ''),
            '网络频段': specs.get('网络频段', ''),
            'SIM卡类型': specs.get('SIM卡类型', ''),
            'WLAN功能': specs.get('WLAN功能', ''),
            '定位导航': specs.get('定位导航', ''),
            '蓝牙': specs.get('蓝牙', ''),
            'NFC': specs.get('NFC', ''),
            '红外功能': specs.get('红外功能', ''),
            '连接与共享': specs.get('连接与共享', ''),
            '机身接口': specs.get('机身接口', ''),
            '其他网络参数': specs.get('其他网络参数', ''),
            '电池类型': specs.get('电池类型', ''),
            '电池容量': specs.get('电池容量', ''),
            '有线充电': specs.get('有线充电', ''),
            '无线充电': specs.get('无线充电', ''),
            '无线反向充电': specs.get('无线反向充电', ''),
            '三防功能': specs.get('三防功能', ''),
            '感应器': specs.get('感应器', ''),
            '音频支持': specs.get('音频支持', ''),
            '视频支持': specs.get('视频支持', ''),
            '多媒体技术': specs.get('多媒体技术', ''),
            '其他功能': specs.get('其他功能', ''),
            '包装清单': specs.get('包装清单', ''),
            '保修政策': specs.get('保修政策', ''),
            '质保时间': specs.get('质保时间', ''),
            '质保备注': specs.get('质保备注', ''),
            '客服电话': specs.get('客服电话', ''),
            '电话备注': specs.get('电话备注', ''),
            '详细内容': specs.get('详细内容', ''),
        }
        
        # 添加计算字段
        row['battery_mah'] = extract_battery_capacity(row['电池容量'])
        row['battery_class'] = classify_battery(row['battery_mah'])
        row['price_class'] = classify_price(row['jd_price'])
        row['charging_watt'] = extract_charging_watt(row['有线充电'])
        row['screen_size_num'] = extract_screen_size(row['屏幕尺寸'])
        row['screen_class'] = classify_screen_size(row['screen_size_num'])
        row['weight_g'] = extract_weight_grams(row['重量'])
        row['weight_class'] = classify_weight(row['weight_g'])
        
        processed_data.append(row)
    
    if processed_data:
        df = pd.DataFrame(processed_data)
        output_path = file_path.replace('.json', '_output.csv')
        df.to_csv(output_path, index=False, encoding='utf-8-sig')
        
        print(f"✅ 转换完成！输出文件: {output_path}")
        print(f"📊 共 {len(df)} 条记录，{len(df.columns)} 个字段")
        
        # 统计预览
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
    else:
        print("⚠️ 没有有效数据可转换")

if __name__ == "__main__":
    file_path = "/Users/Zhuanz1/Documents/简历/coroutine-asio-http-server/examples/python_services/ai-agent-compare-values/graphrag/dataset/phones_zol.json"
    transition_to_csv(file_path)