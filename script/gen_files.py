import argparse
import hashlib
import os
import random
import sys

# 定义大小常量（字节）
MIN_SIZE = 1 * 1024  # 1 KB
MAX_SIZE = 5 * 1024 * 1024  # 5 MB


def generate_random_file(target_dir):
    """生成一个随机大小的文件，返回生成的文件大小（字节）"""
    file_size = random.randint(MIN_SIZE, MAX_SIZE)

    # 生成随机字节内容
    file_content = os.urandom(file_size)

    # 计算内容的 MD5 值并作为文件名
    md5_hash = hashlib.md5(file_content).hexdigest()
    file_path = os.path.join(target_dir, md5_hash)

    # 写入文件
    with open(file_path, "wb") as f:
        f.write(file_content)

    #print(f"成功生成文件: {md5_hash} | 大小: {file_size / 1024:.2f} KB")
    return file_size


def parse_size(size_str):
    """解析用户输入的大小字符串，支持 KB, MB, GB"""
    size_str = size_str.upper().strip()
    try:
        if size_str.endswith("KB"):
            return int(float(size_str[:-2]) * 1024)
        elif size_str.endswith("MB"):
            return int(float(size_str[:-2]) * 1024 * 1024)
        elif size_str.endswith("GB"):
            return int(float(size_str[:-2]) * 1024 * 1024 * 1024)
        else:
            return int(size_str)
    except ValueError:
        print(
            f"错误: 无法解析的大小格式 '{size_str}'。请使用类似 '10MB', '500KB'。"
        )
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(
        description="随机生成 5KB ~ 5MB 的文件，文件名为其 MD5 值。"
    )

    # 创建一个互斥组，确保 -n 和 -s 只能二选一输入
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "-n", "--num", type=int, help="指定需要生成的文件总个数"
    )
    group.add_argument(
        "-s",
        "--size",
        type=str,
        help="指定需要生成的总文件大小 (例如: 10MB, 500KB)",
    )

    # 新增：允许用户指定输出目录，若不指定则默认使用 './generated_files'
    parser.add_argument(
        "-o",
        "--output",
        type=str,
        default="./generated_files",
        help="指定文件保存的目录 (默认: ./generated_files)",
    )

    args = parser.parse_args()

    # 自动创建目标目录（如果不存在的话）
    try:
        os.makedirs(args.output, exist_ok=True)
    except Exception as e:
        print(f"错误: 无法创建或访问目录 '{args.output}'。原因: {e}")
        return

    print(f"文件将生成至目录: {os.path.abspath(args.output)}\n" + "-" * 50)

    # 模式1：指定文件个数
    if args.num is not None:
        if args.num <= 0:
            print("错误: 文件个数必须大于 0")
            return

        total_bytes_generated = 0
        for i in range(args.num):
            print(f"[{i+1}/{args.num}] ", end="")
            total_bytes_generated += generate_random_file(args.output)

        print("-" * 50)
        print(
            f"任务完成！共生成 {args.num} 个文件，总大小: {total_bytes_generated / (1024*1024):.2f} MB"
        )

    # 模式2：指定总大小
    elif args.size is not None:
        target_total_size = parse_size(args.size)
        if target_total_size <= 0:
            print("错误: 总大小必须大于 0")
            return

        current_total_size = 0
        file_count = 0

        print(f"目标总大小: {target_total_size / (1024*1024):.2f} MB")

        # 循环生成，直到当前总大小达到或超过目标大小
        while current_total_size < target_total_size:
            file_count += 1
            #print(f"[{file_count}] ", end="")
            generated_size = generate_random_file(args.output)
            current_total_size += generated_size

        print("-" * 50)
        print(
            f"任务完成！共生成 {file_count} 个文件，实际总大小: {current_total_size / (1024*1024):.2f} MB"
        )


if __name__ == "__main__":
    main()
