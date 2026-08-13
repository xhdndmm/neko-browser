# 架构决策记录 0007：PNG 解码自研、JPEG 封装 libjpeg

- 状态：**Accepted**（2026-08）
- 决策者：架构组 + 用户

## 背景

内容解析里程碑需要图像解码。候选方案：

1. **自研 PNG 解码器**：PNG 格式规范清晰（chunk + CRC-32 + 滤波 + Adam7），
   是学习图像管线、练习解析器安全（长度校验/整数溢出/畸形输入）的理想载体；
   zlib 已作为基础设施依赖，IDAT 解压可直接复用。
2. **封装 libpng**：省事，但 PNG 恰恰是"规范简单到值得自研"的格式，
   且 libpng 接口老、版本差异大。
3. **JPEG 自研**：JPEG 编码体系（Huffman、量化、DCT、YCbCr）远比 PNG 复杂，
   自研成本高且易错 —— 不值得。
4. **封装 libjpeg**：JPEG 采用**自研 + 封装混合**策略：PNG 自研以验证
   图像管线，JPEG 用 libjpeg 封装以控制成本，两者统一暴露
   `neko::image` 接口。

## 决策

- `src/image/` 统一 `Image{width,height,rgba}` 与 `DecodeImage/DecodePng/
  DecodeJpeg`。
- **PNG：自研**。实现 chunk 解析 + CRC-32 校验、全部 5 种滤波、
  Adam7 交错、全部颜色类型/位深（灰度/真彩/调色板 ± alpha），
  zlib 仅用于 IDAT 解压。
- **JPEG：封装 libjpeg**（系统库），对外仍为 `neko::image` 接口。
- GIF/WebP/AVIF 显式返回 NOT IMPLEMENTED（后续按需接入）。

## 后果

- 优点：PNG 管线完全自研可测（测试内含自研 PNG/JPEG 编码器做往返验证，
  16 个图像测试）；JPEG 成本可控；接口统一，GUI 与 CLI 共用。
- 缺点：自研解码器需要防御畸形输入（长度/溢出/超大尺寸均已做边界检查并
  有测试）；PNG 的 16-bit 深度会降为 8-bit 输出（可接受，文档化）。
