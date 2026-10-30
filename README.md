# 串口 Modbus RTU 图形界面上位机（Qt 6）

Qt 6 + C++ 的工业上位机练习项目中的**图形界面版**（`serial_gui`）。

功能：多设备 Modbus RTU 轮询 + 实时数据表 + 阈值报警 + SQLite 数据落地 + 历史查询 + CSV 导出。

轮询期间**界面不卡、无需 `processEvents` 硬抽事件**：串口通信全部跑在工作线程，界面留在主线程，两者只通过信号槽通信（跨线程自动走队列连接）。

## 1. 界面分层

```text
mainwindow.ui / historydialog.ui      界面描述（Qt Designer 可直接双击编辑）
mainwindow.cpp / historydialog.cpp    界面逻辑（主线程，只管显示与人机交互）
serialworker.cpp / serialworker.h     串口通信（工作线程，moveToThread）
devicespec.h                          设备描述结构（界面与工作线程共用）
historydb.cpp / historydb.h           SQLite 数据落地（主线程）
alarmengine.cpp / alarmrule.h         报警判定引擎（纯逻辑：上下限、回差、多规则隔离）
modbus_frame.h / modbus_crc.h         协议层：组帧、解析、CRC16
```

> 项目里的协议层（`modbus_frame.h` / `modbus_crc.h`）与命令行版的主站、从站共用，所以本仓库把它们一起带上，保证能独立编译。

## 2. 构建

需要：CMake ≥ 3.21、Ninja、MinGW 64 位（g++ 13）、Qt 6.11.1（含 `Widgets` / `SerialPort` / `Sql` 模块）。

### Qt Creator（推荐日常开发）

1. 打开本目录的 `CMakeLists.txt`；
2. 选择 **Qt 6.11.1 MinGW 64-bit** Kit（**不要选 MSVC Kit**）；
3. 构建 → 运行。

### 命令行

```powershell
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=G:/study/Qt/6.11.1/mingw_64
cmake --build build
```

> 运行 exe 之前都要把 Qt 的 bin 加进 PATH，否则会以 `0xC0000135`（找不到 DLL）退出：
>
> ```powershell
> $env:PATH = 'G:\study\Qt\6.11.1\mingw_64\bin;' + $env:PATH
> ```

## 3. 使用

1. **串口**下拉选好端口、**波特率**，点「打开串口」；
2. 在「**轮询设备**」表格里配置要轮询的从站：
   - 点「**添加设备**」自动按"上一个地址 + 1"新增一行；
   - 双击单元格可改 **从站地址 / 功能码（`0x03` 或 `0x04`） / 起始地址 / 寄存器数**；
   - 勾掉某行的「**启用**」即让它不参与轮询；
   - 「删除选中」移除一行。
3. 设定 **超时 / 间隔 / 轮数**（轮数 `0` 显示为"不限"= 持续采集），点「**开始轮询**」；
4. 结果出现在「**实时数据**」表：**从站 / 寄存器地址 / 值(十进制) / 值(十六进制) / 状态 / 更新时间**；
5. 想中途停止点「**停止轮询**」。

轮询期间设备表格、报警规则与「添加/删除」都会被禁用，避免"改了配置但实际用的还是旧的"。

**三个语义**（容易混淆，特意写清）：

- **一轮** = 依次访问**所有启用**的设备各一次；
- **间隔** = **两次请求之间**的时间（RS485 共享总线，每帧之间要留间隙）；
- **轮数** = 完整轮数的上限，`0` = 不限。

### 报警

在「**报警规则**」表格里给某个寄存器设阈值（默认已有一条示例规则）：

- 列：**启用 / 从站 / 寄存器地址 / 下限 / 上限 / 回差**；**下限或上限留空 = 不查这一侧**；
- 值超限时：**该行的值与状态列标红**、状态列显示 `超上限` / `超下限`、日志打一条 `⚠ 报警`；
- **只有状态变化时才记录**（产生 / 解除各一次），不会每轮刷屏；
- **回差**用于防抖：超上限报警后，要降到 `上限 − 回差` 才解除（上限 100、回差 5 → 101 报警、96 仍报警、**95 解除**）；
- 勾「**报警时响铃**」→ 报警瞬间 PC 响一声；
- 报警事件随「记录数据到数据库」开关写入数据库，可在「查看历史」里把**类型**切到 **报警记录**查看，也能单独导出 CSV；
- 状态栏右侧显示 `成功/失败/重试 ｜ 报警中N(累计M)`。

## 4. 数据落地（SQLite）

勾选「**记录数据到数据库**」后，每一次成功读取的每个寄存器都会写库（**一行一个值**）：

```sql
CREATE TABLE samples (
  id    INTEGER PRIMARY KEY AUTOINCREMENT,
  ts    TEXT NOT NULL,      -- 'YYYY-MM-DD HH:MM:SS.zzz'
  slave INTEGER NOT NULL,   -- 从站地址
  func  INTEGER NOT NULL,   -- 功能码
  addr  INTEGER NOT NULL,   -- 寄存器地址
  value INTEGER NOT NULL,   -- 原始 16 位值
  raw   TEXT                -- 原始响应帧 hex，便于事后核对
);
-- 索引：(ts) 与 (slave, addr, ts)
```

- **数据库位置**：默认在 **exe 同目录的 `data/serial_gui.db`**；点「**选择数据库位置**」可改，选过之后会**记住**（存在 exe 同目录的 `serial_gui.ini`，明文可读可改）：

  ```ini
  [database]
  dir=你选的目录
  ```

- **WAL 模式**：运行时会看到 `serial_gui.db-wal` / `-shm` 两个配套文件，**正常关闭程序后会自动合并回主库**（强杀进程不会合并）；
- 「**查看历史**」：按 时间起止 / 从站 / 地址 / 条数 过滤，`QTableView` 查看；
- 「**导出 CSV**」：带 UTF-8 BOM，Excel 直接打开中文不乱码。

## 5. 代码约定

- **所有 C++ 源文件以 UTF-8 with BOM 保存**：带 BOM 时 Qt 会按 UTF-8 解释源码里的中文，MinGW 下中文输出正常；
- 界面用 `.ui` 描述（`CMakeLists.txt` 里 `AUTOMOC` + `AUTOUIC`）。**在 Designer 里改界面时不要改控件的 `objectName`**——代码是按 `ui->m_xxx` 引用的。
