"""SpeedTree leaf-card measurement: expresses the card edges and
centres of every camera dump kam_*.txt in the camera frame (right, up,
forward), to see how the cards face the camera.
"""
import numpy as np, glob, math
np.set_printoptions(precision=3, suppress=True)
def read_file(p):
    """The `U card corner x y z` lines of a dump file as {(card, corner): xyz}."""
    U = {}
    for l in open(p):
        w = l.split()
        if w and w[0] == 'U':
            U[(int(w[1]), int(w[2]))] = np.array(list(map(float, w[3:6])))
    return U
def norm(v):
    """`v` scaled to unit length."""
    return v / np.linalg.norm(v)
for p in sorted(glob.glob('kam_*.txt')):
    d = np.array([float(x) for x in p[4:-4].split('_')])
    if np.linalg.norm(d) == 0: d = np.array([1.0, 0, 0])   # a guess: the default
    f = norm(d)
    # camera basis: right = f x z, up = right x f
    z = np.array([0, 0, 1.0])
    r = norm(np.cross(f, z)) if abs(f[2]) < 0.999 else np.array([0, 1.0, 0])
    u = np.cross(r, f)
    B = np.array([r, u, f])   # rows -> coordinates in the camera frame: B @ v
    print(p, 'dir', d)
    U = read_file(p)
    for k in range(4):
        c = [U[(k, i)] for i in range(4)]
        right_dir = c[1] - c[0]; down_vec = c[2] - c[1]; mean = sum(c) / 4
        print('  cluster %d: right %s |%.1f|  down %s |%.1f|  centre %s' % (k, B @ right_dir, np.linalg.norm(right_dir), B @ down_vec, np.linalg.norm(down_vec), B @ mean))
