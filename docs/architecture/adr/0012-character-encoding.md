# 架构决策记录 0012：HTML/文本字符编码采用 WHATWG Encoding

- 状态：**Accepted**（2026-08）
- 决策者：架构组

## 背景

多语言支持要求页面能正确解码各种字符集。真实中文站点（QQ、华为、Bilibili、
CCTV 等）大量使用 GBK/gb18030 编码的 HTML，西欧站点常用 windows-125x/
iso-8859-x，日文站点用 Shift_JIS/EUC-JP。此前引擎假设输入为 UTF-8，遇到
GBK 页面会渲染成乱码。

字符编码是**规范敏感性极高**的领域：标签到字符集的映射、各编码的字节序列
边界、BOM 嗅探优先级、HTML meta 预扫描规则都必须与 WHATWG Encoding 标准
一致，否则不同浏览器之间行为不一致。

## 决策

- 新模块 `neko::base::encoding`（`src/base/`，纯核心层、零第三方依赖）。
- **按 WHATWG Encoding 标准实现**解码器：
  - UTF-8（含截断序列边界处理）、共享 UTF-16 解码器（代理对）。
  - gb18030（2 字节 + 4 字节码点范围）、Big5、Shift_JIS（含 EUDC 私有区）、
    EUC-JP、EUC-KR、ISO-2022-JP（转义状态机）、x-user-defined、replacement。
  - 28 种单字节表（windows-125x/iso-8859-x/koi8-r/koi8-u/macintosh/ibm866/
    x-mac-cyrillic），标签按 WHATWG 标签表映射（GBK/gb2312 → gb18030；
    latin1/ascii/iso-8859-1 → windows-1252；replacement 标签 → U+FFFD）。
  - 无效字节按标准产生 U+FFFD（replacement 错误模式）。
- **HTML 字符集预扫描**：实现 WHATWG 的 prescan 算法（meta charset /
  http-equiv、UTF-16 签名、`<?xml`、注释、generic tag + get-an-attribute）。
- **优先级**：BOM 嗅探 > HTTP `Content-Type` 提示 > HTML 预扫描 > 默认 UTF-8。
- **表数据生成**：`tools/gen_encoding_tables.py` 从 WHATWG 官方索引文件
  （`encoding.spec.whatwg.org` 的 `index-*.txt`）生成 C++ 表
  （`src/base/src/encoding_data.inc` / `encoding_labels.inc`），**提交到仓库**，
  构建无需网络。表规模：gb18030 23940 项、Big5 19782 项（含 >U+FFFF 用
  uint32）、JIS0208 11280 项、JIS0212 8836 项、EUC-KR 23940 项、范围表 207 项。
- **接入点**：`Page::LoadHtml` 统一先 `DetectHtmlCharset` + `DecodeToUtf8`
  再进 HTML 解析器；HTTP 加载路径（BrowserController/CLI）把 `Content-Type`
  字符集提示传入；纯文本加载同样转码。

## 备选方案

- **ICU / iconv**：成熟但引入大型依赖；编码解码器是纯查表 + 状态机，
  自研 + 生成表可保持核心层零依赖且完全受控（依赖政策：不为可自写的简单
  项目特定代码引入依赖）。
- **仅支持 UTF-8**：无法满足多语言真实站点，否决。
- **libiconv 等运行时转换**：编码表来源与错误行为不受控，难以与 WHATWG
  测试对齐，否决。

## 后果

- 多字节编码页面（GBK/Big5/Shift_JIS/EUC-JP/ISO-2022-JP）可正确解码渲染。
- 编码表约 1.6 MiB 源文件提交到仓库（生成代码，由 `gen_encoding_tables.py`
  再生成，明确标识为生成文件，不手工编辑）。
- 解码器只做"编码 → UTF-8"方向；编码（UTF-8 → 其他）留待需要时（表单提交/
  下载文件名等）。
