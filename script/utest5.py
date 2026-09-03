import subprocess
import sys
import time
import threading
from typing import List

def run_program(command_list: List[str], exit_on_error: bool = True) -> subprocess.CompletedProcess:
    """
    通用的程序执行函数。
    
    :param command_list: 包含程序名和参数的列表，例如 ["A", "enable", "1"] 或 ["ping", "google.com"]
    :param exit_on_error: 如果为 True，遇到错误时会直接终止整个 Python 脚本
    :return: CompletedProcess 对象，包含 stdout, stderr 和 returncode 等信息
    """
    if not command_list:
        print("【错误】: 传入的命令列表不能为空！", file=sys.stderr)
        if exit_on_error: sys.exit(1)
        return None

    print(f"-> 正在执行命令: {' '.join(command_list)}")
    
    try:
        # capture_output=True 捕获输出，text=True 以字符串形式读取输出
        # 这里先不设置 check=True，在下方手动处理以提供更清晰的日志
        result = subprocess.run(command_list, text=True, encoding="gbk", errors="ignore")
        
        # 判断程序返回值是否为 0（0 通常代表成功）
        if result.returncode == 0:
            print(f"√ 执行成功！")
            return result
        else:
            print(f"❌ 程序返回错误码: {result.returncode}", file=sys.stderr)
            if result.stderr:
                print(f"【错误详情】:\n{result.stderr.strip()}", file=sys.stderr)
            
            if exit_on_error:
                sys.exit(result.returncode) # 使用程序的错误码退出
            return result

    except FileNotFoundError:
        print(f"【致命错误】: 找不到可执行程序 '{command_list[0]}'，请检查路径或环境变量！", file=sys.stderr)
        if exit_on_error:
            sys.exit(1)
        return None
    except Exception as e:
        print(f"【未知错误】: 执行过程中发生异常: {e}", file=sys.stderr)
        if exit_on_error:
            sys.exit(1)
        return None

protected_disk_path = "E:"  # E:
protected_disk_guid = r"f0e833c9-0000-0000-0000-100000000000"
protected_disk_guid_full = "\\\\?\\Volume{" + protected_disk_guid + "}\\"
journal_disk_guid = r"f0e833c9-0000-0000-0000-f07f16000000"
cdp_tool = r"C:\Users\Administrator\Desktop\123\Release\CdpConsole_Param.exe"
password = "123456"
gen_file_script = r"C:\Users\Administrator\Desktop\123\script\gen_files.py"
check_file_script = r"C:\Users\Administrator\Desktop\123\script\md5_check.py"

if __name__ == "__main__":
       
    print("--- 开启保护 ---")
    run_program([cdp_tool, "enable", protected_disk_guid, journal_disk_guid, password])

    try:
        
        cmd_list = ["python", gen_file_script, "-s", "2GB", "-o", protected_disk_path + "\\"]
        print("--- 写文件 目标2GB ---")
        run_program(cmd_list)

        print("--- 启动线程 写文件 目标2GB ---")
        t = threading.Thread(target=run_program, args=(cmd_list,))
        # 2. 启动线程
        t.start()
        time.sleep(2)

        print("--- 关闭保护 ---")
        run_program([cdp_tool, "stop", protected_disk_guid, password])

        print("--- 等待写文件线程结束 ---")
        t.join() 

        print("--- 卸载保护分区 ---")
        run_program(["mountvol", protected_disk_path, "/P"])
        time.sleep(1)
        print("--- 挂载保护分区 ---")
        run_program(["mountvol", protected_disk_path, protected_disk_guid_full])
        time.sleep(1)
        
        print("--- 检查文件md5 ---")
        run_program(["python", check_file_script, protected_disk_path + "\\"])
    finally:

        print("--- 格式化 ---")
        run_program(["cmd", "/c", "echo.", "|", "format", protected_disk_path, "/fs:ntfs", "/q", "/y"])

    print("\n--- 任务全部结束 ---")
