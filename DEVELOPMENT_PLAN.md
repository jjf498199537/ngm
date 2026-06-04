# NGM 开发计划

> NGM (Next Generation Meeting) — 基于 WebRTC 的小型多人会议系统
> 目标：渐进式从 P2P 演进到 SFU 架构，支持 4-8 人会议

---

## 阶段一：Room Server（信令服务器改造）

**目标**：实现带 Room 概念的信令服务器，让两个客户端通过 Room ID 配对建立 P2P 连接

### 任务清单

- [ ] 搭建项目骨架（目录结构 + BUILD.gn）
- [ ] 实现 Room 数据结构（room.h/cc）— 管理房间内的 peer 列表、角色分配（initiator/joiner）
- [ ] 实现 HTTP 信令接口
  - `GET /sign_in?room=<roomId>&name=<peerName>` — 加入房间
  - `GET /sign_out?room=<roomId>&peer_id=<id>` — 离开房间
  - `POST /message?room=<roomId>&peer_id=<from>&to=<to>` — 发送信令消息
  - `GET /wait?room=<roomId>&peer_id=<id>` — 长轮询等待消息
- [ ] 实现 peer 发现机制 — 新 peer 加入时通知房间内已有成员
- [ ] 实现角色自动分配 — 第一个进入为 initiator，后续为 joiner
- [ ] 单元测试 & 手动联调验证

### 技术选型

- 语言：C++
- 网络：基于 WebRTC rtc_base 的 Socket 封装
- 构建：GN（集成在 WebRTC 源码树内）

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
