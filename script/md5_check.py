import argparse
import hashlib
import os
import sys


def calculate_md5(file_path):
    """分块读取文件并计算 MD5 值，防止大文件撑爆内存"""
    md5_hash = hashlib.md5()
    try:
        with open(file_path, "rb") as f:
            # 每次读取 8192 字节
            for chunk in iter(lambda: f.read(8192), b""):
                md5_hash.update(chunk)
        return md5_hash.hexdigest()
    except Exception as e:
        print(f"错误: 无法读取文件 {file_path}。原因: {e}")
        return None


def verify_directory(target_dir):
    """遍历目录并校验文件名是否与 MD5 一致"""
    if not os.path.isdir(target_dir):
        print(f"错误: 目录 '{target_dir}' 不存在或不是一个有效目录。")
        sys.exit(1)

    print(f"开始校验目录: {os.path.abspath(target_dir)}")
    print("-" * 60)

    count = 0
    success_count = 0
    failed_count = 0
    error_count = 0

    # 遍历目录下的所有项
    for file_name in os.listdir(target_dir):
        file_path = os.path.join(target_dir, file_name)

        # 仅处理文件，跳过子目录
        if os.path.isdir(file_path):
            continue
        
        count +=1
        # 计算实际的 MD5
        actual_md5 = calculate_md5(file_path)

        if actual_md5 is None:
            error_count += 1
            continue

        # 将文件名转换为小写，以便进行不区分大小写的比对
        expected_md5 = file_name.lower().strip()

        if actual_md5 == expected_md5:
            if count % 100 == 0:
                print(f"{count}[ 正常 ] {file_name}")
            success_count += 1
        else:
            print(f"[ 损坏 ] {file_name}")
            print(f"        -> 实际 MD5: {actual_md5}")
            failed_count += 1

    print("-" * 60)
    print("校验完成报告:")
    print(f"  - 校验通过 (无损坏): {success_count} 个")
    print(f"  - 校验失败 (已被篡改): {failed_count} 个")
    if error_count > 0:
        print(f"  - 读取出错 (无法访问): {error_count} 个")

    # 如果有任何文件损坏或出错，返回非零状态码，方便自动化脚本检测
    if failed_count > 0 or error_count > 0:
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="读取指定目录，并将文件名作为预期的 MD5 值进行校验。"
    )
    parser.add_argument(
        "dir",
        type=str,
        nargs="?",
        default="./generated_files",
        help="需要校验的目录路径 (默认: ./generated_files)",
    )

    args = parser.parse_args()
    verify_directory(args.dir)


if __name__ == "__main__":
    main()
