# step 0: 项目骨架 + 构建系统 + 第一个测试
## 1. 目录结构
```text
minicrawler/
├── CMakeLists.txt
├── README.md
├── .gitignore
├── .clang-format
├── src/
│   ├── main.cpp
│   └── util/
│       ├── url.h
│       └── url.cpp          # 第一个模块：URL 解析
├── tests/
│   ├── CMakeLists.txt
│   └── url_test.cpp
├── docs/
│   └── devlog.md            # 学习日志，每一步写点心得
└── scripts/
```

## 2. .gitignore文件：告诉Git哪些文件或目录不需要纳入版本管理
### 常见的需要忽略的内容：
- 编译产物(*.o, build/, *.exe)
- 依赖目录(node_modules/, venv/)
- 敏感信息(.env, 密钥, 数据库密码)
- IDE和系统文件(.vscode/, .idea/, .DS_Store)
- 日志，缓存，临时文件
### 注意：.gitignore 只对还没被 Git 跟踪的文件生效

## 3. CMakeLists.txt文件：CMake的配置文件
### 用来描述项目如何构建，有哪些源文件，编译成什么，依赖哪些第三方库，用什么编译选项
### CMake 本身不编译代码，他根据这个文件生成平台对应的构建文件，再由构建文件去编译，好处是跨平台，易于管理多项目和第三方依赖
### 常用命令：
- cmake_minimum_required(VERSION 3.10) ---- 指定最低CMake版本
- project(名称 LANGUAGES CXX) ---- 定义项目名和语言
- set(CMAKE_CXX_STANDARD 17) ---- 设置C++标准
- add_executable(目标 源文件...) ---- 生成可执行文件
- add_library(目标 STATIC/SHARED 源文件...) ---- 生成静态库/动态库
- target_include_directories(目标 PUBLIC 路径) ---- 头文件搜索路径
- target_link_libraries(目标 PRIVATE库) ---- 链接库
- target_compile_options(目标 PRIVATE选项) ---- 编译选项
- find_package(包名 REQUIRED) ---- 查找第三方库
- add_subdirectory(子目录) ---- 引入子目录
- file(GLOB_RECURSE VAR 通配符) ---- 收集文件列表
- option(名称 "说明" ON) ---- 定义可开关的选项
### CMake以target为中心，依赖关系有传递行：
- PRIVATE：只有目标自己用
- PUBLIC：自己用，依赖他的目标也会继承
- INTERFACE：自己不用，只给依赖者用