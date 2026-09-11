# **键鼠主宰**

你才是键鼠的所有者，没有什么可以屏蔽你对键鼠的掌控。

使用内核级别控制，屏蔽形同虚设



**键盘数据的完整路径（从硬件到用户层）：**

    硬件（键盘） → 内核层驱动 → 内核输入子系统 → 用户态驱动（可选） → 用户层 API → 应用程序
                    ⬆            ⬆                              ⬆
             我的虚拟键鼠位置  我的hook位置                 常见的键鼠模拟位置

**逻辑介绍：**
hook了kbdclass.sys和mouClass.sys的回调函数，以供注入输出，把收到的键鼠数据放入一个1024大小的队列中供普通程序读取



**安装方法：**
需要电脑禁用驱动签名校验，在恢复->高级启动->疑难解答->高级选项->启动设置->F7
或者使用如下bat命令快速进入高级启动：
@echo off
shutdown /r /o /f /t 0


如果你想要跳过上述步骤可以给改驱动购买一年4000RMB的**EV**证书签名😅
非商用根本不可能考虑的事情┑(￣Д ￣)┍



成功进入测试模式后，在管理员模式的cmd中输入devcon.exe install "KMDFDriver1.inf" root\KMDFDriver1
各种文件路径要正确，在生成的解决方案中要有x64的**devcon.exe**（这个东西WDK包自带的文件夹有，我会在该git中附带）

**参考：**
inf文件需要写什么类型等：https://github.com/SenuthLikesCrak/Virtual-HID-Framework-gamepad-example

微软VHF官方用法：https://learn.microsoft.com/zh-cn/windows-hardware/drivers/hid/virtual-hid-framework--vhf-

WDF驱动框架基础：https://www.bilibili.com/video/BV1cu411R7HY/?spm_id_from=333.337.search-card.all.click&vd_source=4f2ab0a07d42ba5c0b331344c619e3b0

一个md编辑器:https://github.com/marktext/marktext/releases/tag/v0.19.1（一直没去学这个怎么写）



**想说的：**
最初只是我想用于刷游戏副本，同时我是狐狸肉作者本人，由于不熟悉github二因素验证规则和硬盘被完全格式化导致以前的账号和数据全部丢失

该代码/程序将永久免费使用
直接禁止将代码（或衍生作品）用于商业盈利，例如：
禁止将代码作为商业产品销售（如闭源软件、付费 API 服务）；
允许个人使用、修改和非商业分发，但商业场景需额外购买授权。
我不是很懂apache2.0等协议，所以这里的声明权重大于协议，其他的均按协议规则使用

**因为我花了很多时间没有收费，所以如果你们直接使用我的代码去收费我会感到不适**

如有解释不周，请见谅。本人零基础







**使用说明**
