#!/usr/bin/env python3
"""Generate a DGX Architecture PPT with embedded diagrams."""

import io
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch
import matplotlib.patheffects as pe
import numpy as np

from pptx import Presentation
from pptx.util import Inches, Pt, Emu
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN
from pptx.util import Inches, Pt

# ─── Color palette ────────────────────────────────────────────────────────────
GREEN  = RGBColor(0x76, 0xB9, 0x00)   # NVIDIA green
DARK   = RGBColor(0x1A, 0x1A, 0x2E)   # deep navy
GRAY   = RGBColor(0x2D, 0x2D, 0x2D)
WHITE  = RGBColor(0xFF, 0xFF, 0xFF)
ACCENT = RGBColor(0x00, 0xB3, 0xE6)   # cyan accent

# ─── Helper: save figure to BytesIO ───────────────────────────────────────────
def fig_to_bytes(fig):
    buf = io.BytesIO()
    fig.savefig(buf, format='png', bbox_inches='tight',
                facecolor=fig.get_facecolor(), dpi=150)
    buf.seek(0)
    plt.close(fig)
    return buf


# ─── Figure 1: DGX H100 单机 GPU 拓扑 ─────────────────────────────────────────
def make_dgx_h100_topo():
    fig, ax = plt.subplots(figsize=(10, 6))
    fig.patch.set_facecolor('#0D1117')
    ax.set_facecolor('#0D1117')
    ax.set_xlim(0, 10); ax.set_ylim(0, 6)
    ax.axis('off')

    ax.text(5, 5.6, 'DGX H100 单机架构  (8× H100 + 4× NVSwitch)',
            ha='center', va='center', fontsize=13, fontweight='bold',
            color='#76B900', fontfamily='DejaVu Sans')

    # GPU positions (2 rows × 4 cols)
    gpu_pos = [(1+i*1.15, 4.2) for i in range(4)] + \
              [(1+i*1.15, 1.8) for i in range(4)]
    gpu_labels = [f'GPU{i}' for i in range(8)]

    for (x, y), lbl in zip(gpu_pos, gpu_labels):
        rect = FancyBboxPatch((x-0.45, y-0.35), 0.9, 0.7,
                              boxstyle="round,pad=0.05",
                              linewidth=1.5, edgecolor='#76B900',
                              facecolor='#1F3A1F')
        ax.add_patch(rect)
        ax.text(x, y, lbl, ha='center', va='center',
                fontsize=9, fontweight='bold', color='white')

    # NVSwitch positions (centre column)
    nsw_pos = [(4.2, 3.0), (5.0, 3.0), (5.8, 3.0), (6.6, 3.0)]
    for (x, y) in nsw_pos:
        rect = FancyBboxPatch((x-0.35, y-0.28), 0.7, 0.56,
                              boxstyle="round,pad=0.05",
                              linewidth=1.5, edgecolor='#00B3E6',
                              facecolor='#0D2233')
        ax.add_patch(rect)
        ax.text(x, y, 'NVSw', ha='center', va='center',
                fontsize=8, color='#00B3E6', fontweight='bold')

    # Draw connections: every GPU -> every NVSwitch
    for gx, gy in gpu_pos:
        for nx, ny in nsw_pos:
            ax.plot([gx, nx], [gy, ny], color='#76B900', alpha=0.25, lw=0.7)

    # Labels
    ax.text(1.5, 0.9, '每块 H100: 700 GB/s NVLink 带宽\n'
            'NVLink 4.0 总带宽: 3.2 TB/s (all-reduce)',
            ha='left', va='center', fontsize=9, color='#AAAAAA')
    ax.text(7.2, 3.0, '4× NVSwitch 3.0\n全互联 Non-blocking',
            ha='left', va='center', fontsize=9, color='#00B3E6')

    return fig_to_bytes(fig)


# ─── Figure 2: NVLink 带宽 vs PCIe 对比柱状图 ──────────────────────────────────
def make_bandwidth_chart():
    fig, ax = plt.subplots(figsize=(8, 4.5))
    fig.patch.set_facecolor('#0D1117')
    ax.set_facecolor('#0D1117')

    labels = ['PCIe 4.0\n(×16)', 'PCIe 5.0\n(×16)', 'NVLink 3.0\n(A100)', 'NVLink 4.0\n(H100)']
    bw     = [32, 64, 600, 900]   # GB/s bidirectional
    colors = ['#555555', '#888888', '#2E7D32', '#76B900']

    bars = ax.bar(labels, bw, color=colors, width=0.5, edgecolor='#333333')
    for bar, val in zip(bars, bw):
        ax.text(bar.get_x() + bar.get_width()/2,
                bar.get_height() + 15,
                f'{val} GB/s', ha='center', va='bottom',
                fontsize=10, fontweight='bold', color='white')

    ax.set_ylabel('双向带宽 (GB/s)', color='#AAAAAA', fontsize=10)
    ax.set_title('GPU 互联带宽对比', color='#76B900', fontsize=13, fontweight='bold')
    ax.tick_params(colors='#AAAAAA')
    ax.spines[['top','right']].set_visible(False)
    for sp in ['bottom','left']:
        ax.spines[sp].set_color('#444444')
    ax.set_ylim(0, 1050)
    ax.yaxis.label.set_color('#AAAAAA')
    ax.tick_params(axis='x', colors='white')
    ax.tick_params(axis='y', colors='#AAAAAA')

    return fig_to_bytes(fig)


# ─── Figure 3: DGX SuperPOD 集群拓扑 ──────────────────────────────────────────
def make_superpod_topo():
    fig, ax = plt.subplots(figsize=(10, 6))
    fig.patch.set_facecolor('#0D1117')
    ax.set_facecolor('#0D1117')
    ax.set_xlim(0, 10); ax.set_ylim(0, 6)
    ax.axis('off')

    ax.text(5, 5.7, 'DGX SuperPOD 集群架构',
            ha='center', va='center', fontsize=13, fontweight='bold',
            color='#76B900')

    # Spine switches (top)
    spine_x = [2.5, 5.0, 7.5]
    for x in spine_x:
        rect = FancyBboxPatch((x-0.55, 4.8), 1.1, 0.55,
                              boxstyle="round,pad=0.05",
                              linewidth=1.5, edgecolor='#FF6B35',
                              facecolor='#2A1500')
        ax.add_patch(rect)
        ax.text(x, 5.07, 'Spine\nSwitch', ha='center', va='center',
                fontsize=8, color='#FF6B35', fontweight='bold')

    # Leaf switches (middle)
    leaf_x = [1.2, 3.0, 4.8, 6.6, 8.4]
    for x in leaf_x:
        rect = FancyBboxPatch((x-0.5, 3.0), 1.0, 0.5,
                              boxstyle="round,pad=0.05",
                              linewidth=1.5, edgecolor='#00B3E6',
                              facecolor='#001A2A')
        ax.add_patch(rect)
        ax.text(x, 3.25, 'Leaf\nSwitch', ha='center', va='center',
                fontsize=8, color='#00B3E6', fontweight='bold')

    # DGX nodes (bottom)
    node_x = [0.8, 1.8, 2.8, 3.8, 4.8, 5.8, 6.8, 7.8]
    for i, x in enumerate(node_x):
        rect = FancyBboxPatch((x-0.38, 1.3), 0.76, 0.7,
                              boxstyle="round,pad=0.05",
                              linewidth=1.2, edgecolor='#76B900',
                              facecolor='#0A1F0A')
        ax.add_patch(rect)
        ax.text(x, 1.65, f'DGX\nH100', ha='center', va='center',
                fontsize=7.5, color='#76B900', fontweight='bold')

    # Spine <-> Leaf connections
    for sx in spine_x:
        for lx in leaf_x:
            ax.plot([sx, lx], [4.8, 3.5], color='#FF6B35', alpha=0.3, lw=0.8)

    # Leaf <-> Node connections (nearest)
    leaf_node_map = {1.2: [0.8,1.8], 3.0: [2.8,3.8], 4.8: [4.8], 6.6: [5.8,6.8], 8.4: [7.8]}
    for lx, nodes in leaf_node_map.items():
        for nx in nodes:
            ax.plot([lx, nx], [3.0, 2.0], color='#00B3E6', alpha=0.4, lw=0.9)

    # Legend
    ax.text(0.2, 0.7, '● Spine: 400G InfiniBand HDR400 / Ethernet',
            color='#FF6B35', fontsize=8)
    ax.text(0.2, 0.4, '● Leaf:  400G InfiniBand / RoCE 互联',
            color='#00B3E6', fontsize=8)
    ax.text(0.2, 0.1, '● DGX H100: 8× H100, 8× 400G HCA',
            color='#76B900', fontsize=8)

    return fig_to_bytes(fig)


# ─── Figure 4: GPU Memory Hierarchy ────────────────────────────────────────────
def make_memory_hierarchy():
    fig, ax = plt.subplots(figsize=(9, 5))
    fig.patch.set_facecolor('#0D1117')
    ax.set_facecolor('#0D1117')
    ax.axis('off')
    ax.set_xlim(0, 9); ax.set_ylim(0, 5)

    ax.text(4.5, 4.7, 'DGX H100 存储层次结构',
            ha='center', va='center', fontsize=13, fontweight='bold',
            color='#76B900')

    layers = [
        # (y_center, height, label, detail, color_edge, color_face)
        (4.0, 0.4, 'L1 Cache / SMEM', '256 KB / SM', '#76B900', '#1A2E1A'),
        (3.3, 0.4, 'L2 Cache', '50 MB', '#76B900', '#1F3A1F'),
        (2.6, 0.4, 'HBM3 显存', '80 GB  @ 3.35 TB/s', '#00B3E6', '#001A2A'),
        (1.9, 0.4, 'NVLink Peer Memory', '640 GB (8-GPU, 900 GB/s)', '#00B3E6', '#001525'),
        (1.2, 0.4, 'CPU DRAM (via PCIe)', '2 TB  @ 64 GB/s', '#AAAAAA', '#1A1A1A'),
        (0.5, 0.4, 'NFS / 并行文件系统', '无限 (慢)', '#555555', '#111111'),
    ]
    for (y, h, lbl, detail, ec, fc) in layers:
        w = 7.5 - (4.0 - y) * 0.3   # pyramid effect
        x0 = 4.5 - w/2
        rect = FancyBboxPatch((x0, y - h/2), w, h,
                              boxstyle="round,pad=0.04",
                              linewidth=1.4, edgecolor=ec, facecolor=fc)
        ax.add_patch(rect)
        ax.text(4.5, y + 0.04, lbl, ha='center', va='center',
                fontsize=9.5, fontweight='bold', color=ec)
        ax.text(4.5, y - 0.17, detail, ha='center', va='center',
                fontsize=8, color='#CCCCCC')

    return fig_to_bytes(fig)


# ─── Figure 5: DGX 产品线对比雷达图 ───────────────────────────────────────────
def make_product_radar():
    categories = ['GPU性能\n(PFLOPS)', 'GPU显存\n(GB)', 'NVLink带宽\n(TB/s)',
                  '节点间带宽\n(GB/s)', '功耗\n(kW)']
    N = len(categories)

    # Normalized values [0,1] for: A100, H100, GH200
    a100 = [0.30, 0.40, 0.40, 0.50, 0.55]
    h100 = [0.70, 0.65, 0.75, 0.80, 0.80]
    gh200= [1.00, 1.00, 1.00, 1.00, 1.00]

    angles = [n / float(N) * 2 * np.pi for n in range(N)]
    angles += angles[:1]

    fig, ax = plt.subplots(figsize=(7, 5.5), subplot_kw=dict(polar=True))
    fig.patch.set_facecolor('#0D1117')
    ax.set_facecolor('#0D1117')

    for vals, color, label in [
        (a100,  '#888888', 'DGX A100'),
        (h100,  '#76B900', 'DGX H100'),
        (gh200, '#00B3E6', 'DGX GH200'),
    ]:
        v = vals + vals[:1]
        ax.plot(angles, v, color=color, linewidth=2, label=label)
        ax.fill(angles, v, color=color, alpha=0.18)

    ax.set_xticks(angles[:-1])
    ax.set_xticklabels(categories, color='white', fontsize=8.5)
    ax.set_yticks([0.25, 0.5, 0.75, 1.0])
    ax.set_yticklabels(['25%','50%','75%','100%'], color='#666666', fontsize=7)
    ax.spines['polar'].set_color('#333333')
    ax.grid(color='#333333', linestyle='--', linewidth=0.6)
    ax.set_title('DGX 产品线能力对比', color='#76B900',
                 fontsize=13, fontweight='bold', pad=20)
    ax.legend(loc='upper right', bbox_to_anchor=(1.35, 1.15),
              facecolor='#1A1A2E', edgecolor='#333333',
              labelcolor='white', fontsize=9)

    return fig_to_bytes(fig)


# ══════════════════════════════════════════════════════════════════════════════
# Build the PPT
# ══════════════════════════════════════════════════════════════════════════════
prs = Presentation()
prs.slide_width  = Inches(13.33)
prs.slide_height = Inches(7.5)

BLANK = prs.slide_layouts[6]   # blank layout

def add_slide(prs):
    return prs.slides.add_slide(BLANK)

def set_bg(slide, color: RGBColor):
    bg = slide.background
    fill = bg.fill
    fill.solid()
    fill.fore_color.rgb = color

def add_text(slide, text, left, top, width, height,
             fontsize=18, bold=False, color=WHITE, align=PP_ALIGN.LEFT,
             italic=False):
    txBox = slide.shapes.add_textbox(
        Inches(left), Inches(top), Inches(width), Inches(height))
    tf = txBox.text_frame
    tf.word_wrap = True
    p = tf.paragraphs[0]
    p.alignment = align
    run = p.add_run()
    run.text = text
    run.font.size = Pt(fontsize)
    run.font.bold = bold
    run.font.italic = italic
    run.font.color.rgb = color
    return txBox

def add_image(slide, buf, left, top, width, height=None):
    if height:
        slide.shapes.add_picture(buf, Inches(left), Inches(top),
                                 Inches(width), Inches(height))
    else:
        slide.shapes.add_picture(buf, Inches(left), Inches(top),
                                 Inches(width))

def add_rect(slide, left, top, width, height, fill_rgb, line_rgb=None, line_pt=0):
    shape = slide.shapes.add_shape(
        1,  # MSO_SHAPE_TYPE.RECTANGLE
        Inches(left), Inches(top), Inches(width), Inches(height))
    shape.fill.solid()
    shape.fill.fore_color.rgb = fill_rgb
    if line_rgb:
        shape.line.color.rgb = line_rgb
        shape.line.width = Pt(line_pt)
    else:
        shape.line.fill.background()
    return shape

# ─── Slide 1: Title ────────────────────────────────────────────────────────────
s1 = add_slide(prs)
set_bg(s1, DARK)

# Green accent bar left
add_rect(s1, 0, 0, 0.12, 7.5, GREEN)
# Decorative top bar
add_rect(s1, 0.12, 0, 13.21, 0.08, GREEN)

add_text(s1, 'DGX 架构深度解析',
         1.0, 1.8, 11, 1.6, fontsize=44, bold=True,
         color=WHITE, align=PP_ALIGN.LEFT)
add_text(s1, 'NVIDIA DGX System Architecture',
         1.0, 3.3, 10, 0.8, fontsize=22, bold=False,
         color=GREEN, align=PP_ALIGN.LEFT)
add_text(s1, '从单节点 H100 到 SuperPOD 集群的全栈互联设计',
         1.0, 4.05, 11, 0.7, fontsize=16, bold=False,
         color=RGBColor(0xAA,0xAA,0xAA), align=PP_ALIGN.LEFT)
add_text(s1, 'GPU Cluster Architecture  |  2026',
         1.0, 6.7, 11, 0.5, fontsize=11,
         color=RGBColor(0x66,0x66,0x66), align=PP_ALIGN.LEFT)

# ─── Slide 2: DGX H100 单机架构图 ─────────────────────────────────────────────
s2 = add_slide(prs)
set_bg(s2, DARK)
add_rect(s2, 0, 0, 13.33, 0.08, GREEN)
add_rect(s2, 0, 0.08, 0.08, 7.42, GREEN)

add_text(s2, 'DGX H100 单机架构', 0.3, 0.15, 12, 0.7,
         fontsize=26, bold=True, color=WHITE)
add_text(s2, '8× H100 SXM5  ·  4× NVSwitch 3.0  ·  全互联 Non-blocking Fabric',
         0.3, 0.75, 12.5, 0.45, fontsize=13, color=GREEN)

buf2 = make_dgx_h100_topo()
add_image(s2, buf2, 0.2, 1.15, 8.8, 5.6)

# Key specs box
add_rect(s2, 9.2, 1.2, 3.9, 5.5, RGBColor(0x12,0x20,0x12),
         line_rgb=GREEN, line_pt=1.2)
specs = [
    ('GPU',        '8× H100 SXM5 80GB'),
    ('GPU算力',    '32 PFLOPS (BF16)'),
    ('显存',       '640 GB HBM3'),
    ('NVLink',     '3.2 TB/s 双向'),
    ('NVSwitch',   '4× 第三代'),
    ('网络HCA',    '8× 400G InfiniBand'),
    ('CPU',        '2× Intel Xeon'),
    ('系统内存',   '2 TB DDR5'),
    ('存储',       '30 TB NVMe SSD'),
    ('TDP',        '10.2 kW'),
]
add_text(s2, 'Key Specs', 9.35, 1.3, 3.6, 0.45,
         fontsize=12, bold=True, color=GREEN)
for i, (k, v) in enumerate(specs):
    add_text(s2, f'{k}:', 9.35, 1.75+i*0.44, 1.4, 0.42,
             fontsize=9.5, bold=True, color=RGBColor(0xAA,0xAA,0xAA))
    add_text(s2, v, 10.7, 1.75+i*0.44, 2.3, 0.42,
             fontsize=9.5, color=WHITE)

# ─── Slide 3: NVLink 带宽对比 ──────────────────────────────────────────────────
s3 = add_slide(prs)
set_bg(s3, DARK)
add_rect(s3, 0, 0, 13.33, 0.08, GREEN)
add_rect(s3, 0, 0.08, 0.08, 7.42, GREEN)

add_text(s3, 'NVLink 与互联技术演进', 0.3, 0.15, 12, 0.7,
         fontsize=26, bold=True, color=WHITE)
add_text(s3, '从 PCIe 到 NVLink 4.0 —— 带宽的数量级提升',
         0.3, 0.75, 12.5, 0.45, fontsize=13, color=GREEN)

buf3 = make_bandwidth_chart()
add_image(s3, buf3, 0.3, 1.2, 7.5, 4.5)

# Right: tech notes
notes = [
    ('NVLink 4.0 特性', [
        '• 每 GPU 18 条 NVLink 链路',
        '• 单向带宽：450 GB/s',
        '• 双向带宽：900 GB/s/GPU',
        '• 全系统 3.2 TB/s（8-GPU）',
    ]),
    ('NVSwitch 3.0', [
        '• 总切换容量：3.6 TB/s',
        '• 延迟 < 1 μs',
        '• All-to-All 无竞争',
        '• SHARP in-network compute',
    ]),
]
y_off = 1.3
for title, items in notes:
    add_rect(s3, 8.1, y_off, 4.9, 0.38, RGBColor(0x1F,0x3A,0x1F),
             line_rgb=GREEN, line_pt=1)
    add_text(s3, title, 8.2, y_off+0.04, 4.7, 0.33,
             fontsize=11, bold=True, color=GREEN)
    y_off += 0.38
    for item in items:
        add_text(s3, item, 8.25, y_off, 4.7, 0.38,
                 fontsize=10, color=WHITE)
        y_off += 0.38
    y_off += 0.2

# ─── Slide 4: SuperPOD 集群拓扑 ───────────────────────────────────────────────
s4 = add_slide(prs)
set_bg(s4, DARK)
add_rect(s4, 0, 0, 13.33, 0.08, GREEN)
add_rect(s4, 0, 0.08, 0.08, 7.42, GREEN)

add_text(s4, 'DGX SuperPOD 集群架构', 0.3, 0.15, 12, 0.7,
         fontsize=26, bold=True, color=WHITE)
add_text(s4, '胖树网络拓扑  ·  400G InfiniBand HDR400  ·  可扩展到数千 GPU',
         0.3, 0.75, 12.5, 0.45, fontsize=13, color=GREEN)

buf4 = make_superpod_topo()
add_image(s4, buf4, 0.15, 1.15, 9.2, 5.7)

# Right panel
add_rect(s4, 9.55, 1.2, 3.6, 5.6, RGBColor(0x0D,0x1A,0x2A),
         line_rgb=ACCENT, line_pt=1.2)
add_text(s4, 'SuperPOD 规格', 9.7, 1.3, 3.3, 0.45,
         fontsize=12, bold=True, color=ACCENT)

spod = [
    '配置：20 × DGX H100',
    'GPU 总数：160 × H100',
    'GPU 算力：~1 ExaFLOPS',
    '节点互联：8× 400G IB/节点',
    '集群存储：≥1 PB',
    'RDMA over InfiniBand',
    '支持 RoCEv2 以太网方案',
    '可横向扩展：多 SuperPOD',
]
for i, line in enumerate(spod):
    add_text(s4, f'• {line}', 9.7, 1.8+i*0.58, 3.3, 0.5,
             fontsize=9.5, color=WHITE)

# ─── Slide 5: 存储层次结构 ─────────────────────────────────────────────────────
s5 = add_slide(prs)
set_bg(s5, DARK)
add_rect(s5, 0, 0, 13.33, 0.08, GREEN)
add_rect(s5, 0, 0.08, 0.08, 7.42, GREEN)

add_text(s5, 'DGX H100 存储与内存层次', 0.3, 0.15, 12, 0.7,
         fontsize=26, bold=True, color=WHITE)
add_text(s5, '多层次存储设计 —— 带宽与容量的完美平衡',
         0.3, 0.75, 12.5, 0.45, fontsize=13, color=GREEN)

buf5 = make_memory_hierarchy()
add_image(s5, buf5, 0.2, 1.15, 9.0, 5.6)

# Right: takeaways
tko = [
    ('HBM3 亮点', '80 GB / GPU，3.35 TB/s，\n比 GDDR6X 快 3×'),
    ('NVLink Pool', 'Peer GPU 直接访问，\n无需 CPU 中转'),
    ('PCIe 5.0', 'CPU↔GPU 传输 64 GB/s,\n主机内存 2 TB'),
    ('NVMe SSD', '30 TB 本地 NVMe,\n顺序读 > 200 GB/s'),
]
y_off = 1.3
for title, body in tko:
    add_rect(s5, 9.4, y_off, 3.7, 1.15, RGBColor(0x10,0x1A,0x10),
             line_rgb=GREEN, line_pt=0.8)
    add_text(s5, title, 9.55, y_off+0.06, 3.5, 0.38,
             fontsize=10.5, bold=True, color=GREEN)
    add_text(s5, body, 9.55, y_off+0.44, 3.5, 0.65,
             fontsize=9, color=WHITE)
    y_off += 1.3

# ─── Slide 6: 产品线对比 + 总结 ────────────────────────────────────────────────
s6 = add_slide(prs)
set_bg(s6, DARK)
add_rect(s6, 0, 0, 13.33, 0.08, GREEN)
add_rect(s6, 0, 0.08, 0.08, 7.42, GREEN)

add_text(s6, 'DGX 产品线对比与总结', 0.3, 0.15, 12, 0.7,
         fontsize=26, bold=True, color=WHITE)
add_text(s6, 'A100 → H100 → GH200 —— 每代性能跨越式提升',
         0.3, 0.75, 12.5, 0.45, fontsize=13, color=GREEN)

buf6 = make_product_radar()
add_image(s6, buf6, 0.1, 1.1, 7.2, 5.7)

# Summary box
add_rect(s6, 7.5, 1.15, 5.65, 5.65, RGBColor(0x10,0x10,0x1E),
         line_rgb=GREEN, line_pt=1.2)
add_text(s6, '核心设计理念', 7.65, 1.25, 5.3, 0.45,
         fontsize=13, bold=True, color=GREEN)

summary = [
    ('Scale-Up',  'NVLink/NVSwitch 构建节点内\n全互联，消除通信瓶颈'),
    ('Scale-Out', 'InfiniBand 高带宽低延迟互联\n扩展至数千 GPU'),
    ('HBM3',      '超高显存带宽匹配 AI 计算\n强度，避免内存墙'),
    ('全栈优化',  'CUDA/NCCL/cuDNN 深度协同，\n最大化硬件利用率'),
    ('总结',      'DGX = GPU + NVLink + NVSwitch\n+ IB + 软件栈的一体化方案'),
]
y_off = 1.75
for k, v in summary:
    add_text(s6, k, 7.65, y_off, 1.5, 0.9,
             fontsize=10, bold=True, color=ACCENT)
    add_text(s6, v, 9.2, y_off, 3.8, 0.9,
             fontsize=9.5, color=WHITE)
    y_off += 0.95

# ─── Save ──────────────────────────────────────────────────────────────────────
out_path = '/home/liujiaqi/conweave-ns3-CBFC+GBN/DGX_Architecture.pptx'
prs.save(out_path)
print(f'Saved: {out_path}')
