# 开发环境配置

## 验证日期
2026-03-07

## 系统环境

### 操作系统
- **OS**: Windows 10/11 (Build 26200)
- **Shell**: PowerShell

### Visual Studio
- **版本**: Visual Studio 2022 Community
- **安装路径**: `C:\Program Files\Microsoft Visual Studio\2022\Community`
- **编译器**: MSVC (Microsoft Visual C++)

### CUDA
- **版本**: CUDA 13.1 (Release 13.1.80)
- **安装路径**: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
- **编译器**: nvcc (NVIDIA CUDA Compiler Driver)
- **环境变量**:
  - `CUDA_PATH`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
  - `CUDA_PATH_V13_1`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.1`
  - `CUDA_PATH_V12_6`: `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.6` (备用)

### OptiX
- **版本**: OptiX SDK 8.0.0
- **安装路径**: `C:\ProgramData\NVIDIA Corporation\OptiX SDK 8.0.0`

## 编码规则

### 构建系统
- 使用CMake作为构建系统
- 目标平台：Windows x64
- 支持CUDA 13.1和OptiX 8.0.0

### CUDA开发
- CUDA计算能力：根据目标GPU设置（建议sm_75+）
- 使用CUDA 13.1 API
- 确保代码与OptiX 8.0.0兼容

### C++标准
- 使用C++17或更高版本
- 与CUDA 13.1兼容的C++特性

### 命令行规则
- 使用Windows PowerShell命令语法
- 多个命令需要分开执行，不使用`&&`连接符（PowerShell中使用`;`或分开执行）

## 路径约定
- 项目根目录：`F:\project\VLR_WF`
- 文档目录：`docs/`
- 使用反斜杠`\`作为Windows路径分隔符（或正斜杠`/`在git中）

## 开发注意事项
1. 确保CUDA和OptiX环境变量已正确设置
2. 使用Visual Studio 2022的开发者命令提示符进行编译
3. CMake配置时需要指定CUDA和OptiX路径
4. 所有GPU代码应使用`.cu`扩展名
5. OptiX程序应使用`.cu`文件并通过OptiX编译器编译
