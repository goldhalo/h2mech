# h2-session-wip —— S2 会话层的半成品（**未接入构建**）

这里放的是 HTTP/2 服务端会话层的第一版实现（`server.hpp` / `server.cpp` /
`test_h2_server.cpp`）。**它目前不可用**，因此不参与编译，也不在 CI 里。

保留它的原因：状态机、流表、读写泵、trailers 的骨架都写完了，问题只卡在一个
nghttp2 接入点；丢掉重写不划算。

## 已知问题（下次从这里接手）

现象：h2c 连接建立、客户端的 HEADERS 帧**确实被解析**（`remote_max_streams`
变成对端的值），但**任何回调都不触发** —— `on_begin_headers` / `on_frame_recv`
一次都没进，业务拿不到请求。

已经排除的：

- 回调签名/注册顺序：用同一份 `libnghttp2_static.a` 写的最小探针（只注册
  `on_frame_recv`，喂 "preface + SETTINGS"）**能正常触发**；
- 成员函数指针 vs 自由函数：换成自由函数同样不触发；
- callbacks 对象生命周期：改成"活到 session 销毁之后"仍然不触发；
- 帧内容合法性：hexdump 确认是合法的 HEADERS（`00 00 14 01 05 00 00 00 01`
  + 合法 HPACK），`mem_recv` 返回 53 = 全量消费；
- `sess_` 指针：init 与 on_read 打印的是同一个地址。

**重点怀疑对象**：`nm` 显示最终可执行文件里有 **2 份**
`T nghttp2_session_mem_recv` —— 说明 nghttp2 被链接了两次（各带独立静态
状态）。虽然理论上回调结构是 POD、两份实现应可互通，但"帧解析正常、回调全空"
正是两份实现互相看不见对方状态的典型表现。

下一步建议（按顺序）：

1. 让 `nm -C <bin> | grep ' T nghttp2_'` 只剩一份（查链接行里
   `libnghttp2_static.a` 是否出现两次：h2mech PRIVATE 链接 + 测试再链接，
   或者 `--start-group` 造成的重复）；
2. 若仍有问题，把 `Session` 的 nghttp2 交互抽成不依赖 netmech 的小类，
   用"喂字节"的纯单测复现（这样能把 netmech 完全排除在外）；
3. 通过之后再补：多流并发、流控窗口、`RST_STREAM`/`GOAWAY` 处理。

## 怎么启用

把三个文件拷回原位：

```
server.hpp        → include/h2mech/h2/
server.cpp        → src/h2/
test_h2_server.cpp→ test/
```

并在 `CMakeLists.txt` 里打开被注释掉的 3 行（`H2MECH_HAS_NGHTTP2` +
`target_link_libraries(h2mech PRIVATE nghttp2_static)`），测试目标也要拿到
nghttp2 的头与宏（见 CMakeLists 里测试段的说明）。

## 里面也顺手修掉的两个坑（值得保留）

1. `nghttp2_session_mem_send` 返回的指针只在下次调用前有效 → 必须拷一份再交给
   `Conn::write`，并把它作为 `ConstBuf::owner` 持有；
2. `Conn::write` 返回 `false` = 本次未受理（队列满）→ 必须自己留 pending，
   在 `on_write` 里续泵，不能原地重试。
