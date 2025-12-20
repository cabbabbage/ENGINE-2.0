import numpy as np
import matplotlib.pyplot as plt
from matplotlib.animation import FuncAnimation, PillowWriter
from mpl_toolkits.mplot3d.art3d import Line3DCollection


# -----------------------------
# Parameters
# -----------------------------

MAX_DEPTH = 4          # tree depth (0 = root only)
BASE_SPACING = 1.0     # overall size of the structure in world units
SCALE_BASE = 3.0       # geometric factor between levels
FRAMES = 120           # total frames in the loop (zoom out then back in)
FPS = 30               # playback framerate
OUTPUT_FILE = "gridpoint_zoom.gif"


# -----------------------------
# Build the hierarchical 3D grid
# -----------------------------

def build_grid_tree(max_depth=MAX_DEPTH, base_spacing=BASE_SPACING, scale_base=SCALE_BASE):
    """
    Build a tree of GridPoint positions.

    Each node has up to 6 children along the axes:
      X±, Y±, Z±

    Children are placed closer to the parent by a factor of scale_base
    each level:
      spacing(level) = base_spacing / (scale_base ** (level + 1))
    """
    nodes = []   # list of (x, y, z, depth)
    edges = []   # list of (parent_index, child_index)

    directions = np.array([
        [-1,  0,  0],
        [ 1,  0,  0],
        [ 0, -1,  0],
        [ 0,  1,  0],
        [ 0,  0, -1],
        [ 0,  0,  1],
    ], dtype=float)

    def recurse(depth, pos):
        idx = len(nodes)
        nodes.append((pos[0], pos[1], pos[2], depth))

        if depth >= max_depth:
            return idx

        spacing = base_spacing / (scale_base ** (depth + 1))

        for d in directions:
            child_pos = pos + spacing * d
            child_idx = recurse(depth + 1, child_pos)
            edges.append((idx, child_idx))

        return idx

    recurse(0, np.array([0.0, 0.0, 0.0], dtype=float))

    nodes = np.array(nodes)  # shape (N, 4) [x, y, z, depth]
    return nodes, edges


nodes, edges = build_grid_tree()


# -----------------------------
# Prepare base geometry arrays
# -----------------------------

# Base positions
base_xyz = nodes[:, :3]       # (N, 3)
depths = nodes[:, 3]          # (N,)

# Build line segments for edges
segments = []
for parent_idx, child_idx in edges:
    p = base_xyz[parent_idx]
    c = base_xyz[child_idx]
    segments.append([p, c])
segments = np.array(segments)  # (M, 2, 3)


# -----------------------------
# Set up matplotlib 3D figure
# -----------------------------

fig = plt.figure(figsize=(6, 6))
ax = fig.add_subplot(111, projection="3d")
fig.tight_layout()

# Background to plain white
fig.patch.set_facecolor("white")
ax.set_facecolor("white")

# Color nodes by depth for a bit of structure
depth_min = depths.min()
depth_max = depths.max()
depth_range = max(1.0, (depth_max - depth_min))
norm_depth = (depths - depth_min) / depth_range
colors = plt.cm.viridis(norm_depth)

scatter = ax.scatter(
    base_xyz[:, 0], base_xyz[:, 1], base_xyz[:, 2],
    c=colors,
    s=8,
    depthshade=True
)

# Lines (edges)
line_collection = Line3DCollection(
    segments,
    colors="black",
    linewidths=0.4,
    alpha=0.7
)
ax.add_collection3d(line_collection)

# Static view settings and limits
ax.set_xlim(-BASE_SPACING, BASE_SPACING)
ax.set_ylim(-BASE_SPACING, BASE_SPACING)
ax.set_zlim(-BASE_SPACING, BASE_SPACING)

# Hide axes, ticks, labels, and grids
ax.set_axis_off()
ax.set_xticks([])
ax.set_yticks([])
ax.set_zticks([])

# Keep aspect ratio cubic
ax.set_box_aspect([1, 1, 1])


# -----------------------------
# Animation function
# -----------------------------

def scale_for_frame(frame_idx, frames=FRAMES, scale_base=SCALE_BASE):
    """
    Scale factor for this frame.

    We want a perfectly looping animation where frame 0 and frame
    (frames - 1) show the same image.

    The simplest way:
      - First half of frames: zoom out from scale 1 to 1 / scale_base.
      - Second half: zoom back in from 1 / scale_base to 1.

    That gives:
      frame 0       -> scale = 1
      middle frame  -> scale = 1 / scale_base
      last frame    -> scale = 1 (same as first)
    """
    if frames <= 1:
        return 1.0

    t = frame_idx / float(frames - 1)  # in [0, 1]

    if t <= 0.5:
        # zoom out
        u = t / 0.5          # in [0, 1]
    else:
        # zoom in
        u = (1.0 - t) / 0.5  # in [0, 1]

    # u goes 0 -> 1 -> 0
    # u = 0  => scale = 1
    # u = 1  => scale = 1 / scale_base
    return scale_base ** (-u)


def update(frame_idx):
    s = scale_for_frame(frame_idx)

    # Scale all positions
    xyz = base_xyz * s
    segs_scaled = segments * s

    # Update scatter
    scatter._offsets3d = (xyz[:, 0], xyz[:, 1], xyz[:, 2])

    # Update line segments
    line_collection.set_segments(segs_scaled)

    return scatter, line_collection


# -----------------------------
# Create and save animation
# -----------------------------

anim = FuncAnimation(
    fig,
    update,
    frames=FRAMES,
    interval=1000.0 / FPS,
    blit=False
)

print(f"Saving animation to {OUTPUT_FILE} (this may take a moment)...")
writer = PillowWriter(fps=FPS)
anim.save(OUTPUT_FILE, writer=writer)
print("Done.")
