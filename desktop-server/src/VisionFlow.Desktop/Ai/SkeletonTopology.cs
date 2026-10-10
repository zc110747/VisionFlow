namespace VisionFlow.Desktop.Ai;

/// <summary>
/// 骨架拓扑定义。
///
/// <para><b>为什么拓扑要数据驱动而不是硬编码</b></para>
/// <para>
/// 关键点数组是**位置索引**（第 0 个是鼻子/手腕，第 1 个是左眼……），
/// 不同模型的关键点数量与语义完全不同：
/// COCO17 人体（yolo11n-pose）与 MediaPipe HAND21 手掌（ai-hand）
/// 的关节表毫无共同点。把连线写成 <c>new[]{0,1},{1,2},...</c>
/// 散在绘制代码里，一旦接入新模型就会画出错误骨架且无人察觉。
/// </para>
/// <para>
/// 这里把每条拓扑声明成「名 + 关节点数 + 连线对」三要素，
/// 绘制前先校验点数是否匹配：不匹配就**跳过不画**，
/// 而不是画出错位的连线（错位骨架比没有骨架更误导）。
/// </para>
/// </summary>
internal sealed record SkeletonTopology(
    string Name,
    int ExpectedPoints,
    (int From, int To)[] Bones)
{
    /// <summary>该拓扑是否适用于给定关键点数。</summary>
    public bool Matches(int pointCount) => pointCount == ExpectedPoints;
}

/// <summary>已知骨架拓扑表。</summary>
internal static class SkeletonTopologies
{
    /// <summary>
    /// COCO 17 点人体骨架（yolo11n-pose 输出顺序）。
    ///
    /// <para>索引表（Ultralytics COCO keypoints 定义）：</para>
    /// <code>
    ///  0 nose            1 left_eye        2 right_eye
    ///  3 left_ear        4 right_ear       5 left_shoulder
    ///  6 right_shoulder  7 left_elbow      8 right_elbow
    ///  9 left_wrist     10 right_wrist    11 left_hip
    /// 12 right_hip      13 left_knee      14 right_knee
    /// 15 left_ankle     16 right_ankle
    /// </code>
    /// </summary>
    public static readonly SkeletonTopology Coco17 = new(
        "COCO-17",
        17,
        [
            // 头部
            (0, 1), (0, 2), (1, 3), (2, 4),
            // 躯干：双肩相连、双髋相连，肩髋成躯干两侧
            (5, 6), (5, 11), (6, 12), (11, 12),
            // 左臂
            (5, 7), (7, 9),
            // 右臂
            (6, 8), (8, 10),
            // 左腿
            (11, 13), (13, 15),
            // 右腿
            (12, 14), (14, 16),
        ]);

    /// <summary>
    /// MediaPipe HAND 21 点手掌骨架（ai-hand 输出顺序）。
    ///
    /// <para>索引表（MediaPipe Hand landmarks）：</para>
    /// <code>
    ///  0 wrist
    ///  1 thumb_cmc   2 thumb_mcp   3 thumb_ip    4 thumb_tip
    ///  5 index_mcp   6 index_pip   7 index_dip   8 index_tip
    ///  9 middle_mcp 10 middle_pip 11 middle_dip 12 middle_tip
    /// 13 ring_mcp   14 ring_pip   15 ring_dip   16 ring_tip
    /// 17 pinky_mcp  18 pinky_pip  19 pinky_dip  20 pinky_tip
    /// </code>
    /// </summary>
    public static readonly SkeletonTopology Hand21 = new(
        "HAND-21",
        21,
        [
            // 手腕 → 各指根
            (0, 1), (0, 5), (0, 9), (0, 13), (0, 17),
            // 拇指链
            (1, 2), (2, 3), (3, 4),
            // 食指链
            (5, 6), (6, 7), (7, 8),
            // 中指链
            (9, 10), (10, 11), (11, 12),
            // 无名指链
            (13, 14), (14, 15), (15, 16),
            // 小指链
            (17, 18), (18, 19), (19, 20),
        ]);

    /// <summary>全部拓扑，按关节点数索引，供自动匹配。</summary>
    private static readonly SkeletonTopology[] All = [Coco17, Hand21];

    /// <summary>
    /// 按实际关键点数选择拓扑。无匹配返回 null（调用方据此跳过绘制）。
    /// </summary>
    public static SkeletonTopology? ForPointCount(int count)
    {
        foreach (var t in All)
        {
            if (t.Matches(count))
            {
                return t;
            }
        }

        return null;
    }
}
