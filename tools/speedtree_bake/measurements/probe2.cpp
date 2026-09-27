// probe2 <spt> <rocking 0/1> <wind> <dx> <dy> <dz> <t1> [t2 ...]
// Prints the cluster tables and the corners of every cluster (the first leaf of the given cluster) after each time.
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "SpeedTreeRT.h"
/// See the usage line above: loads the tree, sets rocking/wind and the camera
/// direction, and prints the cluster tables and cluster corners at each time.
int main(int argc, char** argv)
{
    if (argc < 8) { printf("args\n"); return 1; }
    FILE* f = fopen(argv[1], "rb"); if (!f) { printf("no file\n"); return 1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<unsigned char> v(n); fread(&v[0], 1, n, f); fclose(f);
    int bRock = atoi(argv[2]); float fWiatr = (float)atof(argv[3]);
    float afKam[3] = { 0, -1000, 500 };
    float afDir[3] = { (float)atof(argv[4]), (float)atof(argv[5]), (float)atof(argv[6]) };
    float afLight1[] = { 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
    CSpeedTreeRT::SetNumWindMatrices(4);
    CSpeedTreeRT::SetLightAttributes(0, afLight1);
    CSpeedTreeRT::SetLightState(0, true);
    CSpeedTreeRT* p = new CSpeedTreeRT;
    p->SetWindStrength(1.0f); p->SetLocalMatrices(0, 4); p->SetTextureFlip(true);
    if (!p->LoadTree(&v[0], (unsigned)n)) { printf("LoadTree: %s\n", CSpeedTreeRT::GetCurrentError()); return 1; }
    p->SetBranchLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetLeafLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetFrondLightingMethod(CSpeedTreeRT::LIGHT_STATIC);
    p->SetBranchWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetLeafWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetFrondWindMethod(CSpeedTreeRT::WIND_NONE);
    p->SetNumLeafRockingGroups(1);
    if (!p->Compute(NULL, 1)) { printf("Compute: %s\n", CSpeedTreeRT::GetCurrentError()); return 1; }
    float bb[6]; p->GetBoundingBox(bb);
    p->SetLeafRockingState(bRock != 0); CSpeedTreeRT::SetDropToBillboard(true);
    p->SetLodLimits((bb[5]-bb[2]) * 5.0f, (bb[5]-bb[2]) * 30.0f);
    p->SetLodLevel(1.0f);
    p->SetWindStrength(fWiatr);
    unsigned int ne = 0; const float* tab = p->GetLeafBillboardTable(ne);
    printf("tabela %u\n", ne / 16);
    for (unsigned i = 0; i < ne; i += 4) printf("T %u %u %.4f %.4f %.4f\n", i/16, (i/4)%4, tab[i],tab[i+1],tab[i+2]);
    CSpeedTreeRT::SGeometry g;
    if (afDir[0] != 0 || afDir[1] != 0 || afDir[2] != 0) CSpeedTreeRT::SetCamera(afKam, afDir);
    for (int a = 7; a < argc; ++a)
    {
        float t = (float)atof(argv[a]);
        CSpeedTreeRT::SetTime(t);
        p->GetGeometry(g, SpeedTree_LeafGeometry);
        printf("time %.3f\n", t);
        tab = p->GetLeafBillboardTable(ne);
        for (unsigned i = 0; i < ne; i += 4) printf("U %u %u %.4f %.4f %.4f\n", i/16, (i/4)%4, tab[i],tab[i+1],tab[i+2]);
        for (unsigned k = 0; k < ne / 16; ++k)
            for (unsigned l = 0; l < g.m_sLeaves0.m_usLeafCount; ++l)
                if (g.m_sLeaves0.m_pLeafClusterIndices[l] == k)
                {
                    const float* c = g.m_sLeaves0.m_pLeafMapCoords[l];
                    for (int r = 0; r < 4; ++r) printf("R %u %d %.4f %.4f %.4f\n", k, r, c[r*4],c[r*4+1],c[r*4+2]);
                    break;
                }
    }
    return 0;
}
