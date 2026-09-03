import subprocess
import sys
import os
import time
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

def get_file_names(directory: str) -> list:
    """获取指定目录下当前层级的所有文件名（不含子目录文件）"""
    try:
        # os.listdir 获取目录下所有内容，isfile 过滤出仅文件的部分
        return [name for name in os.listdir(directory) 
                if os.path.isfile(os.path.join(directory, name))]
    except FileNotFoundError:
        print(f"错误：找不到目录 '{directory}'")
        return []
    except PermissionError:
        print(f"错误：没有权限访问目录 '{directory}'")
        return []

def get_current_utc_seconds() -> int:
    """获取当前 UTC 时间的秒级时间戳（整数）"""
    return int(time.time())

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

    #------------------------------------------
        print("--- 写10个文件 ---")
        run_program(["python", gen_file_script, "-n", "10", "-o", protected_disk_path + "\\"])
        
        t1_file_list = get_file_names(protected_disk_path)
        print("--- 睡眠30秒继续执行 ---")
        time.sleep(30)
        t1 = get_current_utc_seconds()
        print("t1:", t1)
        time.sleep(2)

        print("--- 写10个文件 ---")
        run_program(["python", gen_file_script, "-n", "10", "-o", protected_disk_path + "\\"])
        
        t2_file_list = get_file_names(protected_disk_path)
        print("--- 睡眠30秒继续执行 ---")
        time.sleep(30)
        t2 = get_current_utc_seconds()
        print("t2:", t2)
        time.sleep(2)

        print("--- 写5个文件 ---")
        run_program(["python", gen_file_script, "-n", "5", "-o", protected_disk_path + "\\"])

        print("--- 卸载保护分区 ---")
        run_program(["mountvol", protected_disk_path, "/P"])
        
        print("--- 恢复到t1 ---")
        run_program([cdp_tool, "recover", protected_disk_guid, password, str(t1)])

        print("--- 挂载保护分区 ---")
        run_program(["mountvol", protected_disk_path, protected_disk_guid_full])
        time.sleep(1)

        print("--- 检查文件md5 ---")
        run_program(["python", check_file_script, protected_disk_path + "\\"])

        print("--- 检查文件列表 ---")
        t1_file_list_recover = get_file_names(protected_disk_path)
        if t1_file_list == t1_file_list_recover:
            print("两个列表完全相同")
        else:
            print("两个列表不同")
            print("before:", t1_file_list)
            print("after:", t1_file_list_recover)

        print("--- 写5个文件 ---")
        run_program(["python", gen_file_script, "-n", "5", "-o", protected_disk_path + "\\"])
    #------------------------------------------
        print("--- 卸载保护分区 ---")
        run_program(["mountvol", protected_disk_path, "/P"])
        
        print("--- 恢复到t2 ---")
        run_program([cdp_tool, "recover", protected_disk_guid, password, str(t2)])

        print("--- 挂载保护分区 ---")
        run_program(["mountvol", protected_disk_path, protected_disk_guid_full])
        time.sleep(1)

        print("--- 检查文件md5 ---")
        run_program(["python", check_file_script, protected_disk_path + "\\"])

        print("--- 检查文件列表 ---")
        t2_file_list_recover = get_file_names(protected_disk_path)
        if t2_file_list == t2_file_list_recover:
            print("两个列表完全相同")
        else:
            print("两个列表不同")
            print("before:", t2_file_list)
            print("after:", t2_file_list_recover)

        print("--- 写5个文件 ---")
        run_program(["python", gen_file_script, "-n", "5", "-o", protected_disk_path + "\\"])
    #------------------------------------------
    #合并------------------------------------------
        print("--- 初始分支信息 ---")
        run_program([cdp_tool, "branches", protected_disk_guid, password])

        print("--- 第一次手动合并 ---")
        run_program([cdp_tool, "merge", protected_disk_guid, password])
        print("--- 获取分支信息 ---")
        run_program([cdp_tool, "branches", protected_disk_guid, password])
        print("--- 检查文件md5 ---")
        run_program(["python", check_file_script, protected_disk_path + "\\"])
    #------------------------------------------
    finally:

        print("--- 关闭保护 ---")
        run_program([cdp_tool, "stop", protected_disk_guid, password])

        print("--- 格式化 ---")
        run_program(["cmd", "/c", "echo.", "|", "format", protected_disk_path, "/fs:ntfs", "/q", "/y"])

    print("\n--- 任务全部结束 ---")
