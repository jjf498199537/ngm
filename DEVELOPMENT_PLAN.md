# NGM 开发计划

> NGM (Next Generation Meeting) — 基于 WebRTC 的小型多人会议系统
> 目标：渐进式从 P2P 演进到 SFU 架构，支持 4-8 人会议

---

## 阶段一：Room Server（信令服务器改造）

**目标**：实现带 Room 概念的信令服务器，让两个客户端通过 Room ID 配对建立 P2P 连接

### 任务清单

- [ ] 搭建项目骨架（目录结构 + BUILD.gn）
- [ ] 实现 Room 数据结构（room.h/cc）— 管理房间内的 peer 列表、角色分配（initiator/joiner）
- [ ] 实现 HTTP 信令接口（RESTful path 风格，对齐 AppRTCMobile）
  - `POST /join/<roomId>` — 加入房间，Body 携带 `{"name":"<peerName>"}`，返回 clientId、房间状态、是否 initiator
  - `POST /message/<roomId>/<clientId>` — 发送信令消息（SDP/ICE JSON），Server 转发给房间内对端
  - `POST /leave/<roomId>/<clientId>` — 离开房间
- [ ] 实现 WebSocket 消息通道（对齐 AppRTCMobile，替代长轮询）
  - `GET /ws/<roomId>/<clientId>` — WebSocket 升级握手，建立持久双向连接
  - 握手后通过 WebSocket 帧双向推送信令消息（SDP/ICE/peer 状态变化）
- [ ] 实现 peer 发现机制 — 新 peer 加入时通知房间内已有成员
- [ ] 实现角色自动分配 — 第一个进入为 initiator，后续为 joiner
- [ ] 单元测试 & 手动联调验证

### 实现细节：文件与职责

| 文件 | 职责 |
|------|------|
| `server_socket.h/cc` | 基础 Socket 封装（已有），增加连接状态和发送接口 |
| `http_parser.h/cc` | HTTP 请求解析（Method、Path segments、Headers、Body） |
| `websocket_handler.h/cc` | WebSocket 握手（RFC 6455）+ 帧编解码 |
| `room.h/cc` | Room 数据结构（peer 列表、消息路由、角色分配） |
| `main.cc` | epoll 事件循环、请求分发 |

### 实现细节：WebSocket 协议要点

**握手流程**：
1. 客户端发送 `GET /ws/<roomId>/<clientId>` 并带 `Upgrade: websocket` + `Sec-WebSocket-Key` 头
2. Server 取 Key 拼接 magic string `258EAFA5-E914-47DA-95CA-5AB964C80A5`
3. SHA-1 哈希 → Base64 编码 → 作为 `Sec-WebSocket-Accept` 返回
4. 返回 `HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: <hash>\r\n\r\n`

**帧格式（接收）**：
```
 0               1               2               3
 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7 0 1 2 3 4 5 6 7
+-+-+-+-+-------+-+-------------+-------------------------------+
|F|R|R|R| opcode|M| Payload len |    Extended payload length    |
|I|S|S|S|  (4)  |A|     (7)     |             (16/64)           |
|N|V|V|V|       |S|             |   (if payload len==126/127)   |
| |1|2|3|       |K|             |                               |
+-+-+-+-+-------+-+-------------+-------------------------------+
|     Masking-key (0 or 4 bytes)                                |
+-------------------------------+-------------------------------+
|     Payload Data                                              |
+---------------------------------------------------------------+
```
- Client→Server 必须 masked（MASK=1，4 字节 masking key）
- Server→Client 不 mask
- 关键 opcode：Text(0x1)、Binary(0x2)、Close(0x8)、Ping(0x9)、Pong(0xA)

**连接状态机**：
```
ServerDataSocket 状态：
  HTTP_PENDING  → 正在接收 HTTP 头
  HTTP_READY    → HTTP 请求解析完毕（普通 REST 请求，处理后断开）
  WEBSOCKET     → 已完成升级握手，后续按帧协议收发
```

**依赖（WebRTC 内已有，无需外部库）**：
- SHA-1：`rtc_base/message_digest.h` → `rtc::ComputeDigest("sha-1", ...)`
- Base64：`rtc_base/third_party/base64/base64.h`

### 实现细节：请求交互时序

```
iOS 客户端                          Room Server
    |                                    |
    |-- POST /join/<roomId> ------------>|  (HTTP 请求)
    |<-- 200 {clientId, isInitiator} ----|  (HTTP 响应，断开)
    |                                    |
    |-- GET /ws/<roomId>/<clientId> ---->|  (WebSocket 升级)
    |   Upgrade: websocket               |
    |   Sec-WebSocket-Key: xxx           |
    |<-- 101 Switching Protocols --------|  (升级成功，连接保持)
    |                                    |
    |<=== {"type":"offer","sdp":"..."} =>|  (WebSocket 帧，双向)
    |<=== {"type":"candidate",...} =====>|
    |                                    |
    |-- POST /leave/<roomId>/<cid> ----->|  (或发送 Close 帧)
    |<-- 200 OK -------------------------|
```

### 技术选型

- 语言：C++
- 网络：原生 socket + epoll（已实现），不依赖 rtc_base 网络层
- 构建：GN（集成在 WebRTC 源码树内）
- 依赖：仅使用 WebRTC 的 SHA-1/Base64 工具函数和 rtc_base/logging

---

## 阶段二：多人 Mesh P2P（4-8 人）

**目标**：同一 Room 内多人各自建立 P2P 全互联连接

### 任务清单

- [ ] Server 端：支持 Room 内广播 peer 加入/离开事件给所有成员
- [ ] Client 端：Conductor 改造为管理多个 PeerConnection 实例（每个对端一个）
- [ ] Client 端：UI 改造为多画面网格布局
- [ ] 实现动态加入/离开 — 中途加入者与所有已有成员建连
- [ ] 带宽/性能测试 — 验证 4-8 人场景下的可用性

### 已知限制

- Mesh 模式下 N 人需要 N-1 个上行流，8 人时每人上行 7 路 → 带宽压力大
- 此阶段主要验证信令流程和多连接管理的正确性

---

## 阶段三：SFU 架构（服务端媒体转发）

**目标**：Server 接管媒体转发，客户端只上行一路流，Server 选择性转发

### 任务清单

- [ ] 实现服务端 PeerConnection（基于 libwebrtc API 接收客户端上行流）
- [ ] 实现 Router 逻辑 — 接收 RTP 包并转发给同 Room 其他 Consumer
- [ ] Client 改造为 1 send + 1 recv 模式（不再与每个对端单独建连）
- [ ] 支持 Simulcast — 客户端发送多分辨率流，Server 按需转发
- [ ] 实现带宽估计与流控 — 根据下行接收方网络状况选择转发的分辨率
- [ ] 录制能力（可选）— Server 侧保存 RTP 流为文件

### 技术要点

- 需要引用 WebRTC 内部 API（`pc/`、`media/`、`call/` 模块）
- 服务端不做编解码（SFU 而非 MCU），只做 RTP 包路由

---

## 环境与构建

### 前置依赖

- WebRTC 源码（通过 depot_tools/gclient 拉取）
- Linux 开发环境

### 构建方式

```bash
# 在 WebRTC src/ 目录下
gn gen out/Default
ninja -C out/Default ngm:room_server
```

### 代码管理

- WebRTC 源码：`gclient sync` 管理，不修改
- NGM 代码：独立 Git 仓库，位于 `src/ngm/`
- 远程仓库：https://github.com/jjf498199537/ngm

---

## 阶段验证目标

### 阶段一验证

- [ ] **V1.1**：Room Server 启动，能正确接受客户端 sign_in 请求并分配 peer_id 和 room
- [ ] **V1.2**：两个 peerconnection_client（Linux）通过同一 roomId 配对，完成 1v1 视频通话
- [ ] **V1.3**：iOS AppRTCMobile 客户端适配 Room Server 信令协议，成功接入 Room
- [ ] **V1.4**：peerconnection_client（Linux） 与 AppRTCMobile（iOS）跨平台通过同一 Room 建立连接并通话

### 阶段二验证

- [ ] **V2.1**：4 个客户端同时加入同一 Room，两两建连成功
- [ ] **V2.2**：中途加入/离开不影响其他人的连接
- [ ] **V2.3**：8 人房间下带宽和 CPU 可接受（720p 下行流畅）

### 阶段三验证

- [ ] **V3.1**：客户端只建立 1 个 PeerConnection 到 SFU Server，上行 1 路流
- [ ] **V3.2**：SFU 正确将流转发给房间内其他所有人
- [ ] **V3.3**：Simulcast 工作 — 弱网用户收到低分辨率流，强网用户收到高分辨率流
- [ ] **V3.4**：8 人房间下客户端上行带宽降至 Mesh 模式的 1/7

---

## 里程碑

| 阶段 | 核心验证 | 标志性产出 |
|------|----------|------------|
| 一 | iOS AppRTCMobile 通过 Room 与 Linux 端互通 | 跨平台 1v1 通话 |
| 二 | 8 人同时在线 Mesh 通话 | 多人视频会议 MVP |
| 三 | SFU 单路上行多路下行 | 生产级架构 |
