# -*- coding: utf-8 -*-
"""
Fix A 通用版: 深度裁剪 ultralytics YOLOv8 风格 ONNX
- 任意输入尺寸 (从输入 tensor 推断 grid) / 任意类别数 (从输出通道推断)
- 删掉整条 DFL 解码链, 输出:
    out0 = dfl_logits (1, 64, N)   解码前原始 logits
    out1 = scores     (1, nc, N)   sigmoid 后分数
- 数值验证: 参考解码 vs 原模型输出0 的框行, 误差应 <1e-3
用法: python fix_a_auto.py <in.onnx> [out.onnx]
"""
import sys
import onnx
from onnx import helper
import numpy as np
import onnxruntime as ort


def shape_of(g, name):
    for vi in list(g.value_info) + list(g.output) + list(g.input):
        if vi.name == name:
            dims = [d.dim_value for d in vi.type.tensor_type.shape.dim]
            if all(d > 0 for d in dims):
                return dims
    return None


def decode_boxes(dfl, grids, strides, reg_max=16):
    """参考解码: dfl (1,4*reg_max,N) -> (1,4,N) cxcywh 像素
    通道序: [l(reg), t(reg), r(reg), b(reg)]; anchor 序: 级序 P3.., 级内 x 最快"""
    d = dfl[0]
    out = np.zeros((4, d.shape[1]), dtype=np.float32)
    bins = np.arange(reg_max, dtype=np.float64)
    off = 0
    for s, (gw, gh) in zip(strides, grids):
        n = gw * gh
        for a in range(n):
            col = d[:, off + a].astype(np.float64)
            ltrb = np.empty(4)
            for dr in range(4):
                lg = col[dr * reg_max:dr * reg_max + reg_max]
                e = np.exp(lg - lg.max())
                e /= e.sum()
                ltrb[dr] = (e * bins).sum()
            l, t, r, b = ltrb
            gx, gy = a % gw, a // gw
            out[0, off + a] = ((gx + 0.5) + (r - l) * 0.5) * s   # cx
            out[1, off + a] = ((gy + 0.5) + (b - t) * 0.5) * s   # cy
            out[2, off + a] = (l + r) * s                        # w
            out[3, off + a] = (t + b) * s                        # h
        off += n
    return out[None]


def fix_a(in_path, out_path):
    m = onnx.load(in_path)
    m = onnx.shape_inference.infer_shapes(m)
    g = m.graph
    assert len(g.output) == 1, "预期单输出"
    out0 = g.output[0].name

    cat = next(n for n in g.node if out0 in n.output)
    assert cat.op_type == "Concat" and len(cat.input) == 2, f"输出生产者是 {cat.op_type}"
    box_name, score_name = cat.input[0], cat.input[1]

    dfl_reshape = next(n for n in g.node
                       if n.op_type == "Reshape" and any('/dfl/' in o for o in n.output))
    dfl_name = dfl_reshape.input[0]

    shp_box = shape_of(g, dfl_name)
    shp_sc = shape_of(g, score_name)
    print(f"  dfl_logits: {dfl_name}  {shp_box}")
    print(f"  scores    : {score_name}  {shp_sc}")

    # 反向可达剪枝
    keep = {dfl_name, score_name}
    changed = True
    init_names = {i.name for i in g.initializer}
    while changed:
        changed = False
        for n in g.node:
            if n.output and n.output[0] in keep:
                for i in n.input:
                    if i and i not in init_names and i not in keep:
                        keep.add(i); changed = True
    kept = [n for n in g.node if n.output and n.output[0] in keep]
    del g.node[:]
    g.node.extend(kept)

    del g.output[:]
    for nm, shp in ((dfl_name, shp_box), (score_name, shp_sc)):
        assert shp is not None, f"拿不到 {nm} 的 shape"
        g.output.append(helper.make_tensor_value_info(nm, onnx.TensorProto.FLOAT, shp))

    onnx.checker.check_model(m)
    onnx.save(m, out_path)
    print(f"已保存: {out_path}")


def verify(orig_path, new_path):
    so = ort.SessionOptions(); so.log_severity_level = 3
    a = ort.InferenceSession(orig_path, so, providers=["CPUExecutionProvider"])
    b = ort.InferenceSession(new_path, so, providers=["CPUExecutionProvider"])
    ish = [d if isinstance(d, int) else 1 for d in a.get_inputs()[0].shape]
    x = np.random.rand(*ish).astype(np.float32)
    y0 = a.run(None, {"images": x})[0]
    dfl, scores = b.run(None, {"images": x})
    H, W = ish[2], ish[3]
    strides = (8, 16, 32)
    grids = [(W // s, H // s) for s in strides]
    print(f"  输入 {ish}  dfl {dfl.shape}  scores {scores.shape}  原输出 {y0.shape}")

    es = np.max(np.abs(scores - y0[:, 4:4 + scores.shape[1]]))
    print(f"  scores vs 原输出 4.. 行 最大误差 = {es:.3e}")

    boxes = decode_boxes(dfl, grids, strides, reg_max=dfl.shape[1] // 4)
    eb = np.max(np.abs(boxes - y0[:, :4]))
    print(f"  参考解码 vs 原输出 0..3 行 最大误差 = {eb:.3e}")
    assert es < 1e-5 and eb < 1e-3, "对不上"
    print("  ✅ 裁剪正确 + 参考解码数学正确")


if __name__ == "__main__":
    inp = sys.argv[1]
    outp = sys.argv[2] if len(sys.argv) > 2 else inp[:-5] + "_a.onnx"
    print(f"处理: {inp}")
    fix_a(inp, outp)
    print("验证:")
    verify(inp, outp)
