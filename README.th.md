# GPU Toolbox

[English](README.md) · **ภาษาไทย**

เครื่องมือ Win32 native สำหรับวิเคราะห์ภาระงาน GPU ของ NVIDIA บน Windows
พร้อมคำอธิบายตรงไปตรงมาว่า การควบคุม GPU ทำอะไรได้จริงและทำอะไรไม่ได้

พัฒนาและทดสอบบน RTX 3060 Laptop GPU / Windows 11 / MSVC 14.50

## ทำอะไรได้

- **มอนิเตอร์แบบ real-time** — utilization, อุณหภูมิ, กำลังไฟ และ clock ของ GPU
  ผ่าน NVML พร้อมกับ CPU load รวมผ่าน PDH
- **Watch mode** — เฝ้าดู utilization แล้วบันทึกเป็น *episode* (ช่วงที่เกิน
  threshold ต่อเนื่อง) โดยเก็บค่าสูงสุด ค่าเฉลี่ย ระยะเวลา และรายชื่อ process
  ที่จับไว้ ณ จังหวะที่โหลดสูงสุด
- **เลือก GPU ให้แต่ละแอป** — ย้าย OBS ไปใช้ iGPU หรือ dGPU โดยเขียนค่าลง
  `HKCU\Software\Microsoft\DirectX\UserGpuPreferences`
- **ปรับ CPU/IO priority ของ background process** — ลด priority แล้วคืนค่า
  **เดิมของแต่ละตัว** ไม่ใช่ตั้งกลับเป็น `Normal` ทั้งหมด

## สิ่งที่ตั้งใจไม่ทำ

**ไม่มีปุ่ม "optimize GPU" และไม่มีการจำกัด GPU รายprocess**

GPU utilization เป็น *ผลลัพธ์* ของงานที่ถูกส่งเข้าไป ไม่ใช่ค่าที่ API ระดับ
user-space ตัวไหนตั้งเพดานได้ บน Windows คิวของ GPU เป็นของ WDDM scheduler
การบังคับโควตา GPU ต่อ process ต้องเขียน kernel-mode scheduler filter driver

CUDA ก็ช่วยไม่ได้ในเรื่องนี้ เพราะ CUDA context ควบคุมได้เฉพาะ process ที่
สร้างมันขึ้นมา ส่วนแอปเดสก์ท็อปทั่วไป render ผ่าน D3D12/DXGI โดยไม่เคยถือ
CUDA context เลย

`GpuThrottleDemo` แสดงเรื่องนี้ด้วยหลักฐานจริง — มันไล่รายชื่อทุก process
ที่ใช้ GPU อยู่ รายงานทีละกลไกว่าบังคับข้าม process ได้หรือไม่ แล้วรัน
closed-loop throttle ที่ทำงานได้จริงกับ workload ที่ตัวมันเองเป็นเจ้าของ

UI สะท้อนความจริงของฮาร์ดแวร์แทนที่จะซ่อนไว้ กลไกควบคุมแต่ละตัวถูกตรวจสอบ
แยกกันตอนเปิดโปรแกรม ถ้า driver ปฏิเสธตัวไหนก็ปิดปุ่มนั้นพร้อมอธิบายเหตุผล
ในช่อง output

กลไกทั้งสองไม่ใช่สิ่งทดแทนกันได้ และถ้าทดสอบแค่ตัวเดียวจะเข้าใจผิด
ผลจาก RTX 3060 Laptop GPU ที่ใช้พัฒนา:

| กลไก | ผล |
|---|---|
| Power limit (`nvidia-smi -pl`) | ถูกปฏิเสธ — *"not supported in current scope"* แม้รันเป็น admin |
| ล็อก graphics clock (`nvidia-smi -lgc`) | **ใช้ได้** |

บนฮาร์ดแวร์นี้การล็อก clock จึงเป็นวิธีเดียวที่ใช้ได้จริงในการกดความร้อน
และการใช้พลังงาน โดย `ClockLimitSupported()` ตรวจสอบด้วยการลองล็อกจริงที่
ค่าสูงสุดของการ์ดเอง (ซึ่งไม่เปลี่ยนพฤติกรรมอะไร) แล้วดูว่า driver ยอมรับไหม
แทนที่จะเดาจากชื่อรุ่น

## โครงสร้าง

```
gpu-tool/
  GpuToolbox.cpp      Win32 GUI (ไม่ใช้ MFC/Qt ไม่มี dependency ภายนอก)
  GpuCore.{h,cpp}     logic ทั้งหมด NVML ผูกตอน runtime จาก nvml.dll
  CoreTest.cpp        CLI harness ที่เรียก code path เดียวกัน
  GpuThrottleDemo.cpp รายงานความสามารถ + closed-loop self-throttle
  build*.bat          สคริปต์ build ด้วย MSVC

*.ps1                 PowerShell ต้นฉบับ เก็บไว้เทียบผลกับตัว C++
GPU-Guide.txt         คู่มือใช้งานฉบับเต็ม (ภาษาไทย)
```

## สิทธิ์ Administrator

`GpuToolbox.exe` ต้องรันด้วยสิทธิ์ administrator เท่านั้น โดย
`GpuToolbox.manifest` ระบุ `requireAdministrator` และถูกฝังเข้าไปในตัว
binary ตอน link ดังนั้น Windows จะขึ้น UAC ก่อนที่ process จะเริ่มทำงาน
และจะไม่ยอมเปิดเลยถ้าไม่กดยืนยัน — การเช็ค token ตอน runtime ยกระดับสิทธิ์
ให้ process ที่รันไปแล้วไม่ได้

ผลที่ตามมาในทางปฏิบัติ:

- เปิดแบบเงียบๆ จาก Explorer, โฟลเดอร์ Startup หรือ Task Scheduler ธรรมดา
  ไม่ได้ ถ้าต้องการให้เปิดอัตโนมัติ ให้สร้าง scheduled task แล้วติ๊ก
  *Run with highest privileges*
- `/MANIFESTUAC:NO` ใน `build.bat` เป็นสิ่งจำเป็น ไม่ใช่ของประดับ ถ้าไม่ใส่
  linker จะสร้าง trustInfo ของตัวเองเป็น `asInvoker` แล้วชนกับของเรา
  ทำให้ build ล้มเหลวด้วย `manifest authoring error c1010001`
- `CoreTest.exe` ไม่ได้รับผลกระทบ ยังรันได้โดยไม่ต้อง admin จึงเหมาะกว่า
  สำหรับคำสั่งที่แค่อ่านค่าอย่าง `status` และ `list`

## วิธีเปิดใช้งาน

```
cd gpu-tool
dist.bat
```

`dist.bat` จะ build ทั้งสามโปรแกรมแล้วรวมไว้ในโฟลเดอร์ `dist/` พร้อมคู่มือ
และ shortcut ที่สร้างไว้ให้ เปิดผ่าน `dist\GPU Toolbox.lnk` หรือดับเบิลคลิก
`GpuToolbox.exe` ตรงๆ ก็ได้ — Windows จะขึ้น UAC ให้ทั้งสองทาง

`dist/` เป็นชุดที่สมบูรณ์ในตัวเอง ตัว binary ผูกกับ Windows system DLL เท่านั้น
(`COMCTL32`, `SHELL32`, `PDH`, `USER32`, `GDI32`, `ADVAPI32`, `KERNEL32`)
บวกกับ `nvml.dll` ที่มากับ driver ของ NVIDIA จึงไม่ต้องติดตั้ง Visual C++
redistributable ก๊อปโฟลเดอร์นี้ไปเครื่อง Windows เครื่องไหนที่มีการ์ด NVIDIA
ก็รันได้ทันที

หมายเหตุ: `dist/` ถูก git-ignore ไว้ เพราะ build ใหม่ได้เสมอ และ shortcut
ข้างในเก็บ path แบบเต็มของเครื่องที่ build

## การ Build

```
cd gpu-tool
build.bat          -> GpuToolbox.exe
build-test.bat     -> CoreTest.exe
build-demo.bat     -> GpuThrottleDemo.exe
```

ไม่ต้องมี CUDA Toolkit — `nvml.dll` มาพร้อม driver ของ NVIDIA อยู่แล้ว และ
โค้ดโหลดผ่าน `LoadLibrary`/`GetProcAddress` จึงไม่ต้องใช้ `nvml.h` หรือ
import library

สคริปต์ build ชี้ไปที่ `E:\Visual Studio` ถ้า Visual Studio อยู่ที่อื่น
ให้แก้ตัวแปร `VSPATH`

## CLI

```
CoreTest.exe status              สถานะ GPU
CoreTest.exe list                ดู background process (อ่านอย่างเดียว)
CoreTest.exe lower | restore     ลด / คืนค่า CPU+IO priority
CoreTest.exe watch <วินาที> [%]  Watch mode ตามระยะเวลาที่กำหนด

GpuThrottleDemo.exe -t 20 -d 14  รายงานความสามารถ + throttle loop
```

**ข้อควรระวัง:** เวอร์ชัน C++ กับ PowerShell เก็บ state ของ priority คนละไฟล์
(`gpu-bgpriority-state.txt` กับ `.json`) ใช้ตัวไหน lower ก็ต้อง restore ด้วย
ตัวนั้น — ถ้าสลับกัน priority จะไม่ถูกคืนค่า

## การตรวจสอบความถูกต้อง

โค้ด C++ ถูกตรวจเทียบกับ PowerShell ต้นฉบับบนฮาร์ดแวร์จริง ผลออกมาตรงกัน:
จำนวน process ที่จับได้เท่ากัน (48/48) การ save/restore ไม่มีค่าใดผิดพลาดเลย
ทั้งกับ process ที่อยู่ระดับ `AboveNormal`, `High` และ `Idle` และผลลัพธ์
status ตรงกันรวมถึงค่า power limit ที่เป็น `[N/A]`

ข้อสุดท้ายนี้ทำให้เจอบั๊กจริง — `nvmlDeviceGetEnforcedPowerLimit` คืนค่า 85 W
บนการ์ดที่ `nvidia-smi -pl` ตอบว่า *"not supported in current scope"*
ตอนนี้การตรวจความสามารถจึงเปลี่ยนไปใช้ `nvmlDeviceGetPowerManagementLimit`
ซึ่งรายงานถูกต้องว่าไม่มี power limit ที่ตั้งได้
