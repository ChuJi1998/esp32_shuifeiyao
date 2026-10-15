# legacy/ — 历史版本留存

## device_web_ui/

早期「设备自带网页」版本的前端（`index.html` / `app.js` / `style.css`），
原本烧录到 ESP32 的 LittleFS 分区，由设备自带的 HTTP 服务托管。

当前固件已改为**哑设备**架构：设备只采集并上报，页面、存储、规则与 AI 全部跑在
PC 侧服务（[../server](../server)），LittleFS 分区改用于断链补传缓冲。

这个目录仅作对照参考，**不要**再对它执行 `pio run -t uploadfs`。
PC 侧使用的前端在 [`../server/static/`](../server/static)，是在此基础上增加了
AI 农艺助手页、并把 CDN 依赖改为本地文件的版本。
