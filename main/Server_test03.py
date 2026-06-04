#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
ESP32-S3 专用：空气炸锅智能多轮中控 WebSocket 服务器 (气象高级纠错 & 极速版)
- 支持自适应拼音转换，可查询全中国任意城市、多日期（今天、明天、后天）的实时天气。
- 支持台湾腔语音播报 (zh-TW-HsiaoChenNeural / HsiaoYuNeural)，且只发送一次。
- 精细化天气决策逻辑：支持雷雨强对流带雨伞提示、晴朗多云闭口无雨提示、过渡阴雨常规提示。
"""

import os
import json
import wave
import io
import struct
import array
import numpy as np
from pathlib import Path
from typing import Optional, Dict, Any, List
import requests
from datetime import datetime
import argparse
import asyncio
import socket
import tempfile
import warnings
import websockets

# 导入语音合成模块
try:
    import edge_tts
    EDGE_TTS_AVAILABLE = True
except ImportError:
    EDGE_TTS_AVAILABLE = False

# 💥 导入拼音转换模块，用于自动匹配全中国任意城市
try:
    from pypinyin import pinyin, Style
    PYPINYIN_AVAILABLE = True
except ImportError:
    PYPINYIN_AVAILABLE = False

# 忽略底层 PyTorch / Whisper 警告信息
warnings.filterwarnings("ignore", category=UserWarning)

# ============ 配置区域 ============
class Config:
    """配置类"""
    # DeepSeek API配置
    DEEPSEEK_API_KEY = "sk-b2e2358d55884987bef6ccf249c99d06"  # 替换为实际密钥
    DEEPSEEK_API_URL = "https://api.deepseek.com/chat/completions"
    DEEPSEEK_MODEL = "deepseek-chat"
    
    # ESP32 采集音频输入与回传配置 (严格变回 16kHz / 16-bit / 单声道 PCM)
    PCM_SAMPLE_RATE = 16000        # 16kHz
    PCM_SAMPLE_WIDTH = 2           # 16-bit (2字节)
    PCM_CHANNELS = 1               # 单声道
    PCM_START_OFFSET = 0           # 偏移
    PCM_ENDIAN = "little"          # 小端
    
    # 语音识别配置 (极速调优)
    ASR_METHOD = "whisper"
    WHISPER_MODEL_SIZE = "turbo"   # 极速与精度推荐: base 或 turbo (配合faster-whisper)
    
    # 💥【极速参数】Greedy 极速解码搜索
    WHISPER_BEAM_SIZE = 1          
    WHISPER_BEST_OF = 1
    
    # FFmpeg 路径
    FFMPEG_PATH = "ffmpeg"         
    
    # 输出与接收PCM目录
    OUTPUT_DIR = "./output"
    
    # 本地合成语音调试保存目录 (支持自动创建)
    TEMP_DIR = "./temp"
    
    # 💥 TTS 台湾腔女声音色配置 (推荐: zh-TW-HsiaoChenNeural 或 zh-TW-HsiaoYuNeural)
    TTS_VOICE = "zh-CN-XiaoxiaoNeural"
    
    # WebSocket 服务器配置
    WS_HOST = "0.0.0.0"  
    WS_PORT = 8765       
    
    # 静音/无数据传输自动断句触发时间
    SILENCE_TIMEOUT = 0.5
    
    # 最大保留历史上下文对话轮数
    MAX_HISTORY_TURNS = 10
    
    # 【回音阻断器】最小有效振幅阀值 (16bit PCM 理论区间 [-32768, 32767])
    # 低于此振幅说明是静音、微弱电流或喇叭漏音回音，将被服务器直接丢弃，阻断死循环。
    MIN_AMPLITUDE_THRESHOLD = 120
    
    # 【长度阻断器】最小音频流字节大小 (少于 0.3 秒的录音直接忽略，防止短杂音触发)
    MIN_AUDIO_BYTES = 9600

# ============ 上下文对话管理器 ============
class SessionManager:
    """会话管理器 - 以IP为粒度记忆上下文，支持ESP32临时断线/重连保留记忆"""
    def __init__(self, max_turns: int = 10):
        self.sessions: Dict[str, List[Dict[str, str]]] = {}
        self.max_turns = max_turns

    def get_history(self, client_ip: str) -> List[Dict[str, str]]:
        if client_ip not in self.sessions:
            self.sessions[client_ip] = []
        return self.sessions[client_ip]

    def add_message(self, client_ip: str, role: str, content: str):
        history = self.get_history(client_ip)
        history.append({"role": role, "content": content})
        if len(history) > self.max_turns * 2:
            self.sessions[client_ip] = history[2:]

# ============ 语音识别模块 ============
class SpeechRecognizer:
    """语音识别器 - 支持自适应双ASR引擎 (优先使用 C++ 版 faster-whisper)"""
    
    def __init__(self, config: Config):
        self.config = config
        self.whisper_model = None
        self.engine_type = None  # 用于记录当前使用的底层ASR驱动库
    
    def _init_whisper(self):
        """初始化 ASR。会优先尝试加载 C++ 优化的极速版 faster-whisper"""
        import torch
        device = "cuda" if torch.cuda.is_available() else "cpu"
        
        # 1. 优先尝试初始化 C++ 深度优化版的 faster-whisper 引擎 (速度暴增四倍)
        try:
            from faster_whisper import WhisperModel
            print(f"\n[ASR] 成功检测到 faster-whisper 库！正在启动 C++ 极速推理引擎...")
            compute_type = "float16" if device == "cuda" else "int8"
            
            self.whisper_model = WhisperModel(
                self.config.WHISPER_MODEL_SIZE,
                device=device,
                compute_type=compute_type,
                local_files_only=False  # 优先使用本地模型文件，避免在线下载延迟
            )
            self.engine_type = "faster-whisper"
            print(f"[ASR] faster-whisper ({self.config.WHISPER_MODEL_SIZE}) 极速引擎装载完毕！(设备: {device}, 精度: {compute_type})")
            return
        except ImportError:
            pass
            
        # 2. 备用：官方 openai-whisper
        try:
            import whisper
            print(f"\n[ASR] 未检测到 faster-whisper。正在载入官方 openai-whisper 库...")
            self.whisper_model = whisper.load_model(self.config.WHISPER_MODEL_SIZE, device=device)
            self.engine_type = "openai-whisper"
            print(f"[ASR] openai-whisper ({self.config.WHISPER_MODEL_SIZE}) 备用引擎载入完成 (设备: {device})")
        except ImportError:
            print("错误：未安装任何ASR音频识别库！建议安装极速版：pip install faster-whisper")
            raise
    
    def pcm_to_wav(self, pcm_data: bytes) -> bytes:
        if len(pcm_data) > 44 and pcm_data[:4] == b'RIFF' and pcm_data[8:12] == b'WAVE':
            return pcm_data
        
        if self.config.PCM_START_OFFSET > 0:
            pcm_data = pcm_data[self.config.PCM_START_OFFSET:]
            
        even_len = (len(pcm_data) // 2) * 2
        pcm_data = pcm_data[:even_len]
        
        if self.config.PCM_ENDIAN == "big":
            audio_array = array.array('h', pcm_data)
            audio_array.byteswap()
            pcm_data = audio_array.tobytes()
        
        wav_buffer = io.BytesIO()
        with wave.open(wav_buffer, 'wb') as wav_file:
            wav_file.setnchannels(self.config.PCM_CHANNELS)
            wav_file.setsampwidth(self.config.PCM_SAMPLE_WIDTH)
            wav_file.setframerate(self.config.PCM_SAMPLE_RATE)
            wav_file.writeframes(pcm_data)
            
        wav_buffer.seek(0)
        return wav_buffer.read()

    def pcm_to_text_whisper(self, pcm_data: bytes) -> Optional[str]:
        """高级语音识别 (加入 FP16 加速与 Greedy 搜索配置)"""
        try:
            if self.whisper_model is None:
                self._init_whisper()

            with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as tmp_file:
                tmp_path = tmp_file.name
                wav_data = self.pcm_to_wav(pcm_data)
                tmp_file.write(wav_data)
            
            try:
                # 💥 关键优化：重构 ASR 提示词！
                prompt_text = "这是一段关于日常对话、天气查询和空气炸锅控制的中文普通话语音。例如：今天会下雨吗？今天天气怎么样？帮我烘烤一根香蕉。OK开始吧。"
                
                # 分引擎极速解码
                if self.engine_type == "faster-whisper":
                    segments, info = self.whisper_model.transcribe(
                        tmp_path,
                        language="zh",
                        beam_size=self.config.WHISPER_BEAM_SIZE,
                        initial_prompt=prompt_text,
                        temperature=0.0  # 零温度，直接进行最速解码
                    )
                    text = "".join([segment.text for segment in segments]).strip()
                else:
                    import torch
                    device = "cuda" if torch.cuda.is_available() else "cpu"
                    use_fp16 = True if device == "cuda" else False
                    
                    # 💥 物理修复：剔除了上一个版本中 self.whisper_model.transcribe(0 处的语法错误！
                    result = self.whisper_model.transcribe(
                        tmp_path,
                        language="zh",       
                        task="transcribe",   
                        fp16=use_fp16,                                
                        beam_size=self.config.WHISPER_BEAM_SIZE,      
                        best_of=self.config.WHISPER_BEST_OF,
                        temperature=0.0,                              # 极速：关闭温度迭代，不重试
                        initial_prompt=prompt_text,
                    )
                    text = result["text"].strip()
                
                try:
                    from zhconv import convert
                    text = convert(text, 'zh-cn')
                except ImportError:
                    pass
                print(f"[ASR] 语音原始识别结果: {text}")
                return text
                
            finally:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
            
        except Exception as e:
            print(f"Whisper识别失败: {e}")
            return None

    def pcm_to_text(self, pcm_data: bytes) -> Optional[str]:
        if self.config.ASR_METHOD == "whisper":
            return self.pcm_to_text_whisper(pcm_data)
        return None

# ============ 大模型API调用 ============
class LLMClient:
    """智能空气炸锅大模型中控端"""
    
    def __init__(self, config: Config):
        self.config = config
        self.headers = {
            "Authorization": f"Bearer {config.DEEPSEEK_API_KEY}",
            "Content-Type": "application/json"
        }
    
    def chat_with_llm(self, text: str, history: List[Dict[str, str]]) -> Optional[str]:
        """大模型空气炸锅指令流式解析"""
        if not text or text.strip() == "":
            return ""
        
        system_prompt = (
            "你是一个空气炸锅智能控制助手。请根据与用户的多轮对话历史和用户最新指令，智能推测其真实意图并严格输出 JSON 格式的控制命令。\n\n"
            "【安全与不合理食材拒绝规则】：\n"
            "- 如果用户要求的食物或物品【明显不适合】使用空气炸锅烹饪（例如：西瓜、水、汤、饮料、冰淇淋、冰块、塑料、石头等），你必须拒绝执行烹饪设定。\n"
            "- 此时，【绝对不要】输出 'cook' 动作，也不要生成温度和时间，而是将 action 设为 'chat'。\n"
            "- 并在 reply 字段中委婉拒绝并劝阻，回复格式固定为：'对不起，我无法炸{food}，也不推荐哦。' 或给出类似的专业安全建议。\n\n"
            "【输出格式规范】\n"
            "无论用户说什么，你都必须且只能返回一个干净的JSON字符串。不要包含任何Markdown标记（例如 ```json 等），不要有任何前导或后置的聊天性前言。输出结构根据以下3种场景(action)进行智能选择：\n\n"
            
            "1. 烹饪设置场景 (action = 'cook')：\n"
            "只有当用户说“帮我炸/烤/做某样食物”时触发。你需要智能估算合理的温度 and 时间，输出：\n"
            "{\n"
            "  \"action\": \"cook\",\n"
            "  \"food\": \"食材名称\",\n"
            "  \"temp\": \"加热温度(摄氏度数字字符串，如 '180')\",\n"
            "  \"time\": \"加热时间(秒数数字字符串，如 '300')\",\n"
            "  \"funSpeed\": \"风扇转速，必须只能是 'Low', 'Middle', 'High' 之一\",\n"
            "  \"reply\": \"好的，设定温度{temp}摄氏度，风扇转速{风速中文名}，您的{food}预计{time_in_minutes}分钟内炸好！\"\n"
            "}\n"
            "【💥 特别重要细节要求】：\n"
            "- reply 字段中的 {temp} 必须与 JSON 里的 temp 字段保持完全一致。\n"
            "- reply 字段中的 {风速中文名} 必须根据 JSON 里的 funSpeed 字段对应关联翻译：若 funSpeed 为 'High' 对应翻译为 '高'；'Middle' 对应为 '中'；'Low' 对应为 '低'。此处中文只能是：高、中、低。\n"
            "- reply 字段中的 {time_in_minutes} 必须是 JSON 中的 time 字段转换成分钟后的数字（例如 time 字段为 '300' 对应 '5' 分钟；'480' 对应 '8' 分钟）。\n"
            "- 例如，用户说“帮我炸花生米”，你输出：\n"
            "{\"action\": \"cook\", \"food\": \"花生米\", \"temp\": \"180\", \"time\": \"300\", \"funSpeed\": \"High\", \"reply\": \"好的，设定温度180摄氏度，风扇转速高，您的花生米预计5分钟内炸好！\"}\n"
            "如果是其他食物（例如：帮我烘烤一根香蕉，你估算温度160，时间480），你必须输出相匹配的更丰富的播报语 reply 格式为：\"好的，设定温度160摄氏度，风扇转速中，您的香蕉预计8分钟内炸好！\"，其他食物依此类推。\n\n"
            
            "2. 开始工作场景 (action = 'start')：\n"
            "当用户说“OK，开始吧”、“开机”、“开始加热”、“开始工作”等肯定、开始指令时触发。输出：\n"
            "{\n"
            "  \"action\": \"start\",\n"
            "  \"reply\": \"好的，空气炸锅现在开始加热工作，祝您用餐愉快！\"\n"
            "}\n\n"
            
            "3. 普通聊天/询问场景 (action = 'chat')：\n"
            "当用户在问闲聊（如：“今天天气如何”、“推荐吃点什么”）、问候或非指令话语时触发。此时，【绝对不能】输出任何烹饪温度/时间指令，只需做自然的中文语音回复，输出格式：\n"
            "{\n"
            "  \"action\": \"chat\",\n"
            "  \"reply\": \"你的自然对话回复内容（例如：今天天气是晴天，不建议您吃太多油炸食品，但我可以为您推荐炸薯条作为小吃。）\"\n"
            "}\n"
            "注意：在此场景中，不得夹带 food, temp, time, funSpeed 字段，防止单片机误动作。"
        )
        
        messages = [{"role": "system", "content": system_prompt}]
        messages.extend(history)  
        messages.append({"role": "user", "content": text})  
        
        payload = {
            "model": self.config.DEEPSEEK_MODEL,
            "messages": messages,
            "temperature": 0.1,  
            "max_tokens": 300
        }
        
        print(f"[LLM] 正在判断动作意图并下发指令...")
        
        for attempt in range(3):
            try:
                response = requests.post(
                    self.config.DEEPSEEK_API_URL,
                    headers=self.headers,
                    json=payload,
                    timeout=30
                )
                if response.status_code == 200:
                    result = response.json()
                    if 'choices' in result and len(result['choices']) > 0:
                        return result['choices'][0]['message']['content'].strip()
                else:
                    print(f"[LLM] API请求失败 (状态码 {response.status_code})")
            except Exception as e:
                print(f"[LLM] 请求发生异常: {e}")
            if attempt < 2:
                print("[LLM] 准备重试...")
        return None

# ============ 主处理器 ============
class AudioProcessor:
    """空气炸锅中控核心"""
    def __init__(self, config: Config = None):
        self.config = config or Config()
        self.recognizer = SpeechRecognizer(self.config)
        self.llm_client = LLMClient(self.config)
        self.session_manager = SessionManager(max_turns=self.config.MAX_HISTORY_TURNS)
        
        # 创建底层所需的输出接收目录 [1]
        self.received_pcm_dir = Path(self.config.OUTPUT_DIR) / "received_pcm"
        self.received_pcm_dir.mkdir(parents=True, exist_ok=True)
        
        # 创建本地临时调试语音目录 [1]
        self.temp_dir = Path(self.config.TEMP_DIR)
        self.temp_dir.mkdir(parents=True, exist_ok=True)
    
    async def text_to_pcm_async(self, text: str) -> Optional[bytes]:
        """将文字合成为符合 ESP32-S3 播放规格的 16000Hz, 16-bit, 1ch 纯原始 PCM 流 (小智台湾腔版本)"""
        if not EDGE_TTS_AVAILABLE:
            print("[TTS] 警告: 未安装 edge-tts 库，无法生成语音！")
            return None
        
        try:
            with tempfile.NamedTemporaryFile(suffix='.mp3', delete=False) as temp_mp3:
                temp_mp3_path = temp_mp3.name
            with tempfile.NamedTemporaryFile(suffix='.pcm', delete=False) as temp_pcm:
                temp_pcm_path = temp_pcm.name
            
            try:
                # 💥 音色锁定为台湾腔女声，读取Config配置
                taiwanese_voice = self.config.TTS_VOICE
                communicate = edge_tts.Communicate(text, taiwanese_voice)
                await communicate.save(temp_mp3_path)
                
                # 格式：严格改回 (16kHz / 16-bit / 单声道 PCM) 格式 (s16le / pcm_s16le)
                cmd = [
                    self.config.FFMPEG_PATH,
                    "-y",
                    "-i", temp_mp3_path,
                    "-f", "s16le",                  # 恢复 16-bit 有符号 little-endian PCM
                    "-acodec", "pcm_s16le",          # 采用 16-bit 有符号 PCM 编码器
                    "-ar", str(self.config.PCM_SAMPLE_RATE),
                    "-ac", str(self.config.PCM_CHANNELS),
                    temp_pcm_path
                ]
                
                process = await asyncio.create_subprocess_exec(
                    *cmd,
                    stdout=asyncio.subprocess.PIPE,
                    stderr=asyncio.subprocess.PIPE
                )
                await process.communicate()
                
                if process.returncode == 0:
                    with open(temp_pcm_path, 'rb') as f:
                        pcm_bytes = f.read()
                    print(f"[TTS] 成功将回复文本合成为具有台湾腔的 16k/16bit/1ch PCM 音频流: {len(pcm_bytes)} 字节")
                    return pcm_bytes
                else:
                    print("[TTS]  FFmpeg 转码失败！")
                    return None
            finally:
                for path in (temp_mp3_path, temp_pcm_path):
                    if os.path.exists(path):
                        os.unlink(path)
        except Exception as e:
            print(f"[TTS]  语音合成发生异常: {e}")
            return None

    def diagnose_and_validate_pcm(self, pcm_bytes: bytes) -> tuple[bool, float]:
        """PCM音频状态诊断与物理阻断校验"""
        if not pcm_bytes:
            return False, 0.0
            
        even_len = (len(pcm_bytes) // 2) * 2
        num_samples = even_len // 2
        
        # 物理长度门限：若收到低于最小有效长度的杂音音频，直接抛弃 [1]
        if len(pcm_bytes) < self.config.MIN_AUDIO_BYTES:
            print(f" [物理阻断] 音频流过短 ({len(pcm_bytes)} 字节)，自动忽略。")
            return False, 0.0
            
        audio_array = array.array('h')
        audio_array.frombytes(pcm_bytes[:even_len])
        max_val = max(audio_array) if audio_array else 0
        min_val = min(audio_array) if audio_array else 0
        abs_sum = sum(abs(x) for x in audio_array)
        mean_abs_amplitude = abs_sum / num_samples if num_samples > 0 else 0
        zero_ratio = sum(1 for x in audio_array if x == 0) / num_samples if num_samples > 0 else 0
        
        # print("\n" + "📊" * 15)
        print(" [音频信号物理特征分析]")
        print(f"   数据大小: {len(pcm_bytes)} 字节 | 估算时长: {num_samples / self.config.PCM_SAMPLE_RATE:.2f} 秒")
        print(f"   振幅区间: [{min_val}, {max_val}] | 平均振幅: {mean_abs_amplitude:.2f}")
        print(f"   全零数据占比: {zero_ratio * 100:.2f}%")
        
        # 💥 物理阈值拦截门：阻止回音和极低电流麦克风噪音无限回传。
        if mean_abs_amplitude < self.config.MIN_AMPLITUDE_THRESHOLD:
            print(f"\n [物理阻断成功] 音频平均振幅极低 ({mean_abs_amplitude:.1f} < 门限值 {self.config.MIN_AMPLITUDE_THRESHOLD})！")
            print("    判定为单片机喇叭播音产生的回声/漏音或环境噪音，已自动拦截丢弃。")
            # print("📊" * 15 + "\n")
            return False, mean_abs_amplitude
            
        print(" [检测通过] 音频信号振幅正常，开始提交识别。")
        # print("📊" * 15 + "\n")
        return True, mean_abs_amplitude

    def save_pcm_stream_to_file(self, pcm_bytes: bytes, client_id: str) -> Path:
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        pcm_path = self.received_pcm_dir / f"esp32_{client_id}_{timestamp}.pcm"
        with open(pcm_path, 'wb') as f:
            f.write(pcm_bytes)
        return pcm_path

    # 💥 【新增拼音获取逻辑】
    def get_pinyin(self, text: str) -> str:
        """将任意中文城市名称转为小写字母拼音拼装至Seniverse API (若无pypinyin依赖，采用备用常见城市库)"""
        # 去除多余行政后缀
        clean_text = text.replace("省", "").replace("市", "").replace("县", "").replace("区", "")
        if PYPINYIN_AVAILABLE:
            try:
                from pypinyin import pinyin, Style
                py_list = pinyin(clean_text, style=Style.NORMAL)
                return "".join([item[0] for item in py_list]).lower()
            except Exception as e:
                print(f"[Weather] pypinyin 自动转换失败: {e}，启用内置映射备用")
        
        # 内置气象常用核心城市拼音兜底库
        fallback = {
            "九江": "jiujiang", "南昌": "nanchang", "北京": "beijing", "上海": "shanghai",
            "广州": "guangzhou", "深圳": "shenzhen", "武汉": "wuhan", "长沙": "changsha",
            "杭州": "hangzhou", "南京": "nanjing", "成都": "chengdu", "西安": "xian",
            "重庆": "chongqing", "吉安": "jian", "赣州": "ganzhou", "上饶": "shangrao",
            "宜春": "yichun", "景德镇": "jingdezhen", "萍乡": "pingxiang", "新余": "xinyu",
            "鹰潭": "yingtan", "抚州": "fuzhou"
        }
        return fallback.get(clean_text, "jiujiang")

    # 💥 【新增城市与日期自适应解析逻辑】
    def extract_city_and_day(self, text: str) -> tuple[str, str, int]:
        """从 ASR 文字中智能提取出：城市名、日期字眼(今天/明天/后天) 以及心知天气的 daily 数组索引"""
        # 1. 确定预报日期与对应数组索引 (0:今天, 1:明天, 2:后天)
        if "后天" in text:
            day_str = "后天"
            day_index = 2
        elif "明天" in text:
            day_str = "明天"
            day_index = 1
        else:
            day_str = "今天"
            day_index = 0
            
        # 2. 提取城市名称（通过剔除常见修饰词和多余字提取出城市核心中文名）
        temp = text
        for w in ["今天", "明天", "后天", "怎么样", "会下雨吗", "的天气", "天气", "降雨", "气温", "度数", "雨", "下雪", "刮风", "的", "问问", "查询", "？", "。"]:
            temp = temp.replace(w, "")
            
        # 剔除省份（直辖市除外，防止北京被错误切空）
        provinces = ["江西", "湖南", "湖北", "广东", "广西", "浙江", "江苏", "四川", "陕西", "安徽", "福建", "河北", "河南", "山东", "山西", "辽宁", "吉林", "黑龙江", "云南", "贵州", "青海", "甘肃", "海南", "台湾", "新疆", "西藏", "内蒙古", "宁夏", "内蒙"]
        for p in provinces:
            temp = temp.replace(p, "")
            
        # 剥离最后的行政单位后缀
        temp = temp.replace("省", "").replace("市", "").replace("县", "").replace("区", "")
        temp = temp.strip()
        
        # 兜底默认城市
        city_name = "九江"
        if temp:
            # 智能提取前 2~4 个字符作为核心城市名 (如: "南昌", "景德镇")
            city_name = temp[:4]
            
        return city_name, day_str, day_index

    # 💥 【新增气象通用查询接口及判定逻辑】
    def fetch_weather_by_city(self, city_name: str, day_str: str, day_index: int) -> Optional[str]:
        """根据城市拼音与日期索引向心知天气发送API查询，返回整理后的友好中文播报段"""
        city_pinyin = self.get_pinyin(city_name)
        url = f"https://api.seniverse.com/v3/weather/daily.json?key=SEbXzWJjeuODCTzgR&location={city_pinyin}&language=zh-Hans&unit=c&start=0&days=3"
        try:
            print(f"[Weather] 正在调用天气API | 城市: {city_name} ({city_pinyin}) | 日期: {day_str} ...")
            response = requests.get(url, timeout=10)
            if response.status_code == 200:
                data = response.json()
                results = data.get("results", [])
                if results:
                    daily = results[0].get("daily", [])
                    if len(daily) > day_index:
                        target_day = daily[day_index]
                        text_day = target_day.get("text_day", "晴")
                        text_night = target_day.get("text_night", "阴")
                        high = target_day.get("high", "29")
                        low = target_day.get("low", "22")
                        
                        # 转换天气汉字修饰词，保证播报读起来更自然
                        day_status = "阴天" if text_day == "阴" else ("晴天" if text_day == "晴" else text_day)
                        night_status = "阴天" if text_night == "阴" else ("晴天" if text_night == "晴" else text_night)
                        
                        # 💥 【精细天气提示词高级优化】
                        # 情况 1：白天和晚上都是雷阵雨，属于必然降水情况，提示“出门请带好雨伞” 
                        if "雷阵雨" in text_day and "雷阵雨" in text_night:
                            will_rain_suffix = "，出门请带好雨伞"
                        # 情况 2：白天和晚上都是晴天或多云，属于无雨极佳情况，说完温度即停止，不带任何降雨提示
                        elif text_day in ["晴", "多云"] and text_night in ["晴", "多云"]:
                            will_rain_suffix = ""
                        # 情况 3：其他过渡气象情况（如白天阴天/小雨，晚上阴天/小雨等）提示“可能会下雨了”
                        else:
                            will_rain_suffix = "，可能会下雨了"
                            
                        summary = f"白天{day_status}，晚上{night_status}，气温 {low} 到 {high} 度{will_rain_suffix}"
                        return summary
            print(f"[Weather] 调用天气API响应失败，状态码: {response.status_code}")
        except Exception as e:
            print(f"[Weather] 请求天气接口异常: {e}")
        return None

    def process_pcm_data_sync(self, pcm_bytes: bytes, client_ip: str, client_id: str) -> Optional[Dict[str, Any]]:
        """主入口：处理接收到的 PCM 音频"""
        try:
            # 1. 物理噪声/回音拦截诊断
            is_valid, amp = self.diagnose_and_validate_pcm(pcm_bytes)
            if not is_valid:
                return None  

            pcm_file_path = self.save_pcm_stream_to_file(pcm_bytes, client_id)
            
            # 2. 语音转文字
            recognized_text = self.recognizer.pcm_to_text(pcm_bytes)
            if not recognized_text or not recognized_text.strip():
                print("[ASR] 未能检测到任何有效的人声文本。自动舍弃不调用大模型。")
                return None
            
            # 💥 ASR 层面高级语义与同音字二次校准 (双重防御)
            corrections = {
                "像米": "下雨",
                "相米": "下雨",
                "象米": "下雨",
                "下鱼": "下雨",
                "砸": "炸",
                "榨": "炸",
                "扎": "炸",
                "空客": "空气",
                "空客炸锅": "空气炸锅",
                "空气榨锅": "空气炸锅"
            }
            for wrong, right in corrections.items():
                recognized_text = recognized_text.replace(wrong, right)
            print(f"[ASR] 经过中文声母与气象词二次纠偏后的最终文本: '{recognized_text}'")
            
            # 💥 【天气自适应提取与查询逻辑】
            weather_hint = ""
            weather_keywords = ["天气", "下雨", "降雨", "气温", "度数", "雨", "下雪", "刮风", "冷不冷", "热不热"]
            if any(kw in recognized_text for kw in weather_keywords):
                # 1. 自适应提取：城市名字、日期字符、API数组索引 [2]
                city_name, day_str, day_index = self.extract_city_and_day(recognized_text)
                
                # 2. 获取目标城市和日期的天气段落
                weather_info = self.fetch_weather_by_city(city_name, day_str, day_index)
                
                if weather_info:
                    # 确定省份前缀，保持用户表达的原生性 (例如 "江西九江" / "江西南昌")
                    province_prefix = ""
                    provinces_list = ["江西", "湖南", "湖北", "广东", "广西", "浙江", "江苏", "四川", "陕西", "安徽", "福建", "河北", "河南", "山东", "山西", "辽宁", "吉林", "黑龙江", "云南", "贵州", "青海", "甘肃", "海南", "台湾", "新疆", "西藏", "内蒙古", "宁夏", "内蒙"]
                    for p in provinces_list:
                        if p in recognized_text:
                            province_prefix = p
                            break
                    
                    # 智能拼接显示与播报地名
                    display_city = f"{province_prefix}{city_name}" if (province_prefix and province_prefix not in city_name) else city_name
                    full_weather_text = f"{display_city}{day_str}{weather_info}"
                    
                    # 将动态实况信息组装为提示词注入到大模型输入端
                    weather_hint = (
                        f"\n\n[系统天气预报指令：用户正在询问天气信息，当前天气接口查询到的实况结果为: \"{full_weather_text}\"。"
                        "请直接将上述天气信息整理并填入 JSON 的 'reply' 字段，动作设置为 'chat'。"
                        "特别注意：不要包含任何食物、烹饪控制字段（如food, temp, time, funSpeed）。输出格式必须为: {\"action\": \"chat\", \"reply\": \"{你的回复文本}\"}]"
                    )
            
            # 3. 提取会话历史
            history = self.session_manager.get_history(client_ip)
            
            # 4. 结合上下文提请大模型
            llm_query_text = recognized_text + weather_hint
            llm_response = self.llm_client.chat_with_llm(llm_query_text, history)
            
            # 5. 防御性 JSON 解析
            try:
                clean_json = llm_response.strip()
                if clean_json.startswith("```json"):
                    clean_json = clean_json[7:]
                if clean_json.endswith("```"):
                    clean_json = clean_json[:-3]
                clean_json = clean_json.strip()
                
                command_data = json.loads(clean_json)
            except Exception as e:
                print(f"[LLM] JSON 解析失败: {e}。原始返回:\n{llm_response}")
                command_data = {
                    "action": "chat",
                    "reply": "抱歉，刚刚没有听清，能麻烦您再说一次吗？"
                }
            
            # 6. 保存上下文信息
            self.session_manager.add_message(client_ip, "user", recognized_text)
            self.session_manager.add_message(client_ip, "assistant", llm_response)
            
            result = {
                "file_path": str(pcm_file_path),
                "recognized_text": recognized_text,
                "command_data": command_data,
                "time": datetime.now().isoformat()
            }
            
            # 保存文本版易读报告
            report_path = Path(self.config.OUTPUT_DIR) / f"{pcm_file_path.stem}_report.txt"
            with open(report_path, 'w', encoding='utf-8') as f:
                f.write(f"【控制中控报告 (IP:{client_ip})】\n{'='*35}\n")
                f.write(f"原话: {recognized_text}\n\n")
                f.write(f"决策: {json.dumps(command_data, ensure_ascii=False, indent=2)}\n")
            return result
            
        except Exception as e:
            print(f"中控处理链路发生崩溃: {e}")
            return None

    # ============ WebSocket 核心交互 ============
    async def handle_ws_client(self, websocket):
        """处理ESP32客户端接入 (状态锁控制)"""
        client_address = websocket.remote_address
        client_ip = client_address[0]
        client_id = f"{client_address[1]}"
        print(f"\n [WS] ESP32 客户端已建立连接: {client_address}")
        
        pcm_buffer = bytearray()
        last_print_len = 0  
        
        # 通道自落锁状态机变量
        is_recording = True  
        
        try:
            while True:
                try:
                    message = await asyncio.wait_for(websocket.recv(), timeout=self.config.SILENCE_TIMEOUT)
                    
                    if isinstance(message, bytes):
                        if is_recording:
                            pcm_buffer.extend(message)
                            if len(pcm_buffer) - last_print_len >= 10240:
                                print(f" 正在接收音频流... 当前累计: {len(pcm_buffer) / 1024:.1f} KB", end="\r")
                                last_print_len = len(pcm_buffer)
                        else:
                            pass
                    
                    elif isinstance(message, str):
                        cmd = message.strip().upper()
                        if cmd in ("START", "RESET"):
                            pcm_buffer.clear()
                            last_print_len = 0
                            is_recording = True  # 重新拉开录音通道锁，接收新一轮音频
                            print(f"\n [{client_address}] 收到开始/重置信号，开启录音通道接收...")
                        
                        elif cmd in ("STOP", "END"):
                            print(f"\n [{client_address}] 收到手动终止信号")
                            if is_recording and len(pcm_buffer) > 0:
                                is_recording = False  # 处理前落锁，阻止接收后续多余波形 [1]
                                await self.process_stream_and_reply(websocket, bytes(pcm_buffer), client_ip, client_id)
                                pcm_buffer.clear()  
                                last_print_len = 0
                                is_recording = True  # 💥 【极其关键修复】多轮对话解锁，允许同一个长连接下继续听下一句话！
                                print(f"[{client_address}] 录音通道解锁成功，正在监听下一条语音命令...")
                                
                except asyncio.TimeoutError:
                    if is_recording and len(pcm_buffer) > 0:
                        print(f"\n 1.5 秒无新数据，自动断句，正在中控决策中...")
                        is_recording = False  # 决策前落锁，物理切断单片机音频回音 [1]
                        await self.process_stream_and_reply(websocket, bytes(pcm_buffer), client_ip, client_id)
                        pcm_buffer.clear()
                        last_print_len = 0
                        is_recording = True  # 💥 【极其关键修复】超时决策发送完毕后重新解锁录音，常开接收！
                        print(f"[{client_address}] 录音通道自动解锁成功，正在监听下一条语音命令...")
                        
        except websockets.exceptions.ConnectionClosed:
            print(f" [WS] 会话连接正常释出。 (ESP32 单片机单次流传输后释放属于正常机制)")
        except Exception as e:
            print(f"\n❌ [{client_address}] 异常: {e}")
        finally:
            if is_recording and len(pcm_buffer) > 0:
                print(f"\n [{client_address}] 残留音频防丢失本地备份...")
                await asyncio.to_thread(self.process_pcm_data_sync, bytes(pcm_buffer), client_ip, f"{client_id}_rescue")

    async def process_stream_and_reply(self, websocket, pcm_bytes: bytes, client_ip: str, client_id: str):
        """流处理并下发音频流及JSON命令 (保障只发一次)"""
        result = await asyncio.to_thread(self.process_pcm_data_sync, pcm_bytes, client_ip, client_id)
        
        if result is None:
            return

        if "command_data" in result:
            command_data = result["command_data"]
            action = command_data.get("action", "chat")
            reply_text = command_data.get("reply", "")
            
            # 2. 语音合成 (TTS)
            print(f" 正在对回复文本进行语音合成 (TTS): '{reply_text}'")
            tts_pcm_bytes = await self.text_to_pcm_async(reply_text)
            
            # 保存本地调试文件
            if tts_pcm_bytes:
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                tts_save_path = self.temp_dir / f"tts_reply_{client_id}_{timestamp}.pcm"
                try:
                    with open(tts_save_path, 'wb') as tts_file:
                        tts_file.write(tts_pcm_bytes)
                    print(f"  合成播报16位PCM音频已成功保存至本地调试目录 -> {tts_save_path}")
                except Exception as e:
                    print(f"  [警告] 保存本地播报16位PCM文件失败: {e}")
            
            response_json = {
                "status": "success",
                "action": action,
                "reply": reply_text,
                "has_audio_reply": tts_pcm_bytes is not None
            }
            
            # ✅ 如果是烹饪指令，把 cook 字段提升到顶层，并做类型转换
            if action == "cook":
                # funSpeed 字符串 → 整数映射（与 ESP32 fan_speed_t 枚举一致：High=0, Mid=1, Low=2）
                fun_speed_map = {"High": 0, "Middle": 1, "Low": 2}
                fun_speed_str = command_data.get("funSpeed", "Low")
                
                response_json["food"] = command_data.get("food", "")
                response_json["temp"] = float(command_data.get("temp", 0))       # 字符串 → 浮点数
                response_json["time"] = int(command_data.get("time", 0))          # 字符串 → 整数(秒)
                response_json["funSpeed"] = fun_speed_map.get(fun_speed_str, 2)   # 字符串 → 整数枚举
                
            try:
                # 🚀 阶段 1：发送控制指令 JSON (只发送一次，不重复发) [1]
                await websocket.send(json.dumps(response_json, ensure_ascii=False))
                print(f"  [第一阶段] 成功发送控制指令 JSON: {json.dumps(response_json, ensure_ascii=False)}")
                
                # 🚀 阶段 2：发送语音播报 PCM 原始音频流 (只发送一次) [1]
                if tts_pcm_bytes:
                    await websocket.send(tts_pcm_bytes)
                    print(f"  [第二阶段] 成功发送合成语音原始 PCM 流 ({len(tts_pcm_bytes)} 字节)。")
            except Exception as e:
                print(f"  向 ESP32 回发数据时失败: {e}")
        else:
            err_json = {"status": "error", "message": "大模型智能决策故障。"}
            try:
                await websocket.send(json.dumps(err_json, ensure_ascii=False))
            except Exception as e:
                print(f"  回发故障信息失败: {e}")

    def get_local_ip(self) -> str:
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            ip = s.getsockname()[0]
            s.close()
            return ip
        except Exception:
            return "127.0.0.1"

    async def start_ws_server(self):
        local_ip = self.get_local_ip()
        print("\n" + "="*60)
        print("空气炸锅中控 WebSocket 智能交互服务器已启动！")
        print(f"本地测试地址:  ws://127.0.0.1:{self.config.WS_PORT}")
        print(f"局域网接入地址: ws://{local_ip}:{self.config.WS_PORT}")
        print("="*60 + "\n")
        
        async with websockets.serve(self.handle_ws_client, self.config.WS_HOST, self.config.WS_PORT):
            await asyncio.Future()

# ============ 主入口 ============
if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="空气炸锅智能多轮控制服务端")
    parser.add_argument("--api-key", help="DeepSeek API密钥")
    args = parser.parse_args()
    
    config = Config()
    if args.api_key: 
        config.DEEPSEEK_API_KEY = args.api_key
    
    processor = AudioProcessor(config)
    processor.recognizer._init_whisper()
    try:
        asyncio.run(processor.start_ws_server())
    except KeyboardInterrupt:
        print("\n 服务器已手动关闭。")
