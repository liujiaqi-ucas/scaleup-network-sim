import pandas as pd
import glob
import os

# 1. 定义列名（对应你 Bash 脚本生成的 10 列）
cols = [
    'Protocol', 'Traffic', 'MsgSize', 'BER', 
    'Flows', 'Errors', 'Retrans', 'Avg_FCT', 'P99_FCT', 'JCT'
]

# 2. 定义哪些列是“身份标签”（不参与数值计算）
index_cols = ['Protocol', 'Traffic', 'MsgSize', 'BER']

def main():
    # 查找当前目录下所有以 .csv 结尾的文件
    # 如果你想更精准，可以改成 glob.glob('results_*.csv')
    csv_files = glob.glob('*.csv')
    
    # 过滤掉可能已经存在的平均值结果文件，避免自己加自己
    csv_files = [f for f in csv_files if f != 'final_average.csv']

    if len(csv_files) < 2:
        print(f"找到的文件太少 ({len(csv_files)}个)，无法计算平均值。")
        return

    print(f"正在处理以下文件: {csv_files}")

    # 读取并设置索引
    df_list = []
    for f in csv_files:
        try:
            df = pd.read_csv(f, names=cols).set_index(index_cols)
            df_list.append(df)
        except Exception as e:
            print(f"读取文件 {f} 出错: {e}")

    # 3. 核心计算：对齐求和并除以文件数量
    df_avg = sum(df_list) / len(df_list)

    # 4. 导出结果
    df_avg.reset_index().to_csv('final_average.csv', index=False, header=False)
    print("-" * 30)
    print("成功！平均值已保存至: final_average.csv")

if __name__ == "__main__":
    main()