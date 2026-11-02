# Packet logger (development)

Logs the client's outgoing packets, for mapping the network protocol while developing mods.
Off by default.

| mod | host | part |
|---|---|---|
| `net_probe` | client | the whole feature |

## Use

In SDGO Mod Manager click **显示开发信息** (show developer info), switch on the packet logger for
the client and start the game. Packets go to `sdgo\logs\sdgo.log`. Switch it off the same way.
Keep it off while using [FRP connectivity](frp-distribution.md): both hook `sendto`.

中文：在 Manager 点“显示开发信息”后打开客户端的发包日志，启动游戏后查看 `sdgo\logs\sdgo.log`；
使用 FRP 联机时保持关闭。

## Settings

| key | default | meaning |
|---|---|---|
| `MaxPerOpcode` | 60 | packets logged per opcode |
| `DumpBytes` | 64 | bytes dumped per packet |

## How it works

Hooks the client's TCP send (`net.send_packet`) and UDP `sendto` and logs the first
`MaxPerOpcode` packets of every opcode with a hex dump of `DumpBytes` bytes.
