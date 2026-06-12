/*
 * Asset validation harness for the Vita port's format loaders.
 *
 * Recursively scans the given directories, parses every .rmesh file with
 * the RMesh loader and every .b3d file with the B3D loader, and reports
 * aggregate statistics. Exits non-zero if any file fails to parse, so CI
 * catches loader regressions against the real game data.
 *
 * Build (host):
 *   gcc -O2 -Wall -o validate_assets vita/tools/validate_assets.c \
 *       vita/src/formats/rmesh.c vita/src/formats/b3d.c
 */

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../src/formats/b3d.h"
#include "../src/formats/rmesh.h"

static struct {
    unsigned rmeshOk, rmeshFail;
    unsigned b3dOk, b3dFail;
    unsigned long long surfaces, vertices, triangles, entities;
    unsigned long long b3dMeshNodes, b3dVertices, b3dTriangles;
} stats;

static int failures;

static const char *extOf(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot ? dot + 1 : "";
}

static int extEq(const char *name, const char *ext) {
    const char *e = extOf(name);
    while (*e && *ext) {
        char a = (char)((*e >= 'A' && *e <= 'Z') ? *e + 32 : *e);
        if (a != *ext) return 0;
        e++;
        ext++;
    }
    return *e == '\0' && *ext == '\0';
}

static void tallyNode(const B3DNode *n) {
    if (!n) return;
    if (n->mesh) {
        stats.b3dMeshNodes++;
        stats.b3dVertices += n->mesh->vertexCount;
        for (uint32_t i = 0; i < n->mesh->triSetCount; i++) {
            stats.b3dTriangles += n->mesh->triSets[i].triangleCount;
        }
    }
    for (uint32_t i = 0; i < n->childCount; i++) {
        tallyNode(n->children[i]);
    }
}

static void checkFile(const char *path) {
    char err[256];

    if (extEq(path, "rmesh")) {
        RMesh *m = rmeshLoadFile(path, err, sizeof(err));
        if (!m) {
            printf("FAIL rmesh %s: %s\n", path, err);
            stats.rmeshFail++;
            failures = 1;
            return;
        }
        stats.rmeshOk++;
        stats.surfaces += m->surfaceCount;
        stats.entities += m->entityCount;
        for (uint32_t i = 0; i < m->surfaceCount; i++) {
            stats.vertices += m->surfaces[i].vertexCount;
            stats.triangles += m->surfaces[i].triangleCount;
        }
        rmeshFree(m);
    } else if (extEq(path, "b3d")) {
        B3DModel *m = b3dLoadFile(path, err, sizeof(err));
        if (!m) {
            printf("FAIL b3d   %s: %s\n", path, err);
            stats.b3dFail++;
            failures = 1;
            return;
        }
        stats.b3dOk++;
        tallyNode(m->root);
        b3dFree(m);
    }
}

static void walk(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        printf("FAIL stat  %s\n", path);
        failures = 1;
        return;
    }
    if (S_ISREG(st.st_mode)) {
        checkFile(path);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;

    DIR *d = opendir(path);
    if (!d) {
        printf("FAIL open  %s\n", path);
        failures = 1;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }
        char child[4096];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
        walk(child);
    }
    closedir(d);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir-or-file>...\n", argv[0]);
        return 2;
    }
    for (int i = 1; i < argc; i++) {
        walk(argv[i]);
    }

    printf("\nRMesh: %u ok, %u failed\n", stats.rmeshOk, stats.rmeshFail);
    printf("  surfaces=%llu vertices=%llu triangles=%llu entities=%llu\n",
           stats.surfaces, stats.vertices, stats.triangles, stats.entities);
    printf("B3D:   %u ok, %u failed\n", stats.b3dOk, stats.b3dFail);
    printf("  mesh nodes=%llu vertices=%llu triangles=%llu\n",
           stats.b3dMeshNodes, stats.b3dVertices, stats.b3dTriangles);

    if (stats.rmeshOk + stats.rmeshFail + stats.b3dOk + stats.b3dFail == 0) {
        printf("no assets found - check the paths\n");
        return 2;
    }
    return failures ? 1 : 0;
}
