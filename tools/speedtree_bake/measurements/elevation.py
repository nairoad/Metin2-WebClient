"""SpeedTree leaf-card measurement: reads the corner dumps
el_<elevation>.txt of the baker for elevations -90..90 and fits each card
corner as a + b cos(el) + c sin(el), printing the fit error - how the leaf
cards turn with the camera elevation.
"""
import numpy as np, math
np.set_printoptions(precision=3, suppress=True, linewidth=150)
def read_file(p):
    """The `U card corner x y z` lines of a dump file as {(card, corner): xyz}."""
    U = {}
    for l in open(p):
        w = l.split()
        if w and w[0] == 'U':
            U[(int(w[1]), int(w[2]))] = np.array(list(map(float, w[3:6])))
    return U
elevations = [-90, -75, -60, -45, -30, -15, 0, 15, 30, 45, 60, 75, 90]
T = {e: read_file('el_%d.txt' % e) for e in elevations}
# for cluster 0, corners 0..3: the x,y,z coordinates as a function of elevation
for k in range(2):
    for r in range(4):
        print('cluster %d corner %d' % (k, r))
        for e in elevations:
            print('   el %4d  %s' % (e, T[e][(k, r)]))
# fit: v(el) = a + b*cos(el) + c*sin(el)
print('--- fit a + b cos + c sin (max error) ---')
for k in range(4):
    for r in range(4):
        X = np.array([[1, math.cos(math.radians(e)), math.sin(math.radians(e))] for e in elevations])
        Y = np.array([T[e][(k, r)] for e in elevations])
        C, _, _, _ = np.linalg.lstsq(X, Y, rcond=None)
        err = np.abs(X @ C - Y).max()
        print('cluster %d corner %d: a %s b %s c %s  error %.3f' % (k, r, C[0], C[1], C[2], err))
