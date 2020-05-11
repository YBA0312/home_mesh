# home_mesh
ESP32 [ESP-MDF](https://github.com/espressif/esp-mdf)

在 [编程指南](https://docs.espressif.com/projects/esp-mdf/zh_CN/latest/index.htmll) -> [快速入门](https://docs.espressif.com/projects/esp-mdf/zh_CN/latest/get-started/index.html) 中，详细介绍了环境的搭建。

## 配置

### /main/main.h

```c
#define VERSION "x.x" // 可自定义的版本
#define CONFIG_ROUTER_SSID "xxx" // WiFi SSID
#define CONFIG_ROUTER_PASSWORD "xxx" // WiFi PASSWORD
#define CONFIG_MESH_ID "000001" // MESH ID (可不更改)
#define CONFIG_MESH_PASSWORD "12345678" // MESH PASSWORD (可不更改)
#define CONFIG_SERVER_IP "192.168.x.x" // TCPServer的IP地址
#define CONFIG_SERVER_PORT 8080 // TCPServer的端口
```

### /main/main.c

```c
char OTA_FileUrl[255] = "http://192.168.1.53:8070/ota.bin"; // 默认的bin文件地址
```

## 通讯例子

只是个示例，可以重写或自行增删改，在以下函数中处理

```c
static void node_read_task(void *arg)
```

### 格式

使用json格式（在处理代码里图方便直接用字符串了，可以用cJSON制作）

#### 发送

{"dest_addr":"`目标mac地址，全FF为广播`","data":{"cmd":"`指令`","data":"`数据`"}}

#### 接收

{"src_addr":"`来源mac地址`","data":{"cmd":"`指令`","data":"`数据`"}}

### 查询版本

{"dest_addr":"FF:FF:FF:FF:FF:FF","data":{"cmd":"version"}}

### 重启

{"dest_addr":"FF:FF:FF:FF:FF:FF","data":{"cmd":"restart"}}

### OTA升级

{"dest_addr":"FF:FF:FF:FF:FF:FF","data":{"cmd":"ota"}

{"dest_addr":"FF:FF:FF:FF:FF:FF","data":{"cmd":"ota","data":"http://192.168.1.53:8070/ota.bin"}}

data表示获取bin文件的地址，无"data"时使用默认地址。

编译完成后，可以`cd build`到build目录下，

使用`python -m http.server 8070`便捷的http服务进行OTA升级操作。