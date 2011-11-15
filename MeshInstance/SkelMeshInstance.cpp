#include "Core.h"
#include "UnrealClasses.h"

#if RENDERING

#include "MeshInstance.h"
#include "UnMesh.h"				//?? should be removed

#include "SkeletalMesh.h"

#include "GlWindow.h"
#include "UnMathTools.h"
#include "TypeConvert.h"		//?? used to get CVec3/CQuat from FMeshBone

// debugging
//#define SHOW_INFLUENCES		1
#define SHOW_TANGENTS			1
//#define SHOW_ANIM				1
#define SHOW_BONE_UPDATES		1
//#define PROFILE_MESH			1
//#define TICK_SECTIONS			1

#define SORT_BY_OPACITY			1

#if TICK_SECTIONS
#undef SORT_BY_OPACITY				// to not obfuscate section indices
#endif


struct CMeshBoneData
{
	// static data (computed after mesh loading)
	int			BoneMap;			// index of bone in animation tracks
	CCoords		RefCoords;			// coordinates of bone in reference pose (unused!!)
	CCoords		RefCoordsInv;		// inverse of RefCoords
	int			SubtreeSize;		// count of all children bones (0 for leaf bone)
	// dynamic data
	// skeleton configuration
	float		Scale;				// bone scale; 1=unscaled
	int			FirstChannel;		// first animation channel, affecting this bone
	// current pose
	CCoords		Coords;				// current coordinates of bone, model-space
	CCoords		Transform;			// used to transform vertex from reference pose to current pose
#if USE_SSE
	CCoords4	Transform4;			// SSE version
#endif
	// data for tweening; bone-space
	CVec3		Pos;				// current position of bone
	CQuat		Quat;				// current orientation quaternion
};


// transformed vertex (cutoff version of CSkelMeshVertex)
struct CSkinVert
{
	CVecT		Position;
	CVecT		Normal, Tangent, Binormal;
};


#define ANIM_UNASSIGNED			-2
#define MAX_MESHBONES			512
#define MAX_MESHMATERIALS		256


/*-----------------------------------------------------------------------------
	Interface
-----------------------------------------------------------------------------*/

int CSkelMeshInstance::GetAnimCount() const
{
	if (!Animation) return 0;
	return Animation->Sequences.Num();
}


const char *CSkelMeshInstance::GetAnimName(int Index) const
{
	guard(CSkelMeshInstance::GetAnimName);
	if (Index < 0) return "None";
	assert(Animation);
	return Animation->Sequences[Index].Name;
	unguard;
}


UUnrealMaterial *CSkelMeshInstance::GetMaterial(int Index, int *PolyFlags)
{
	guard(CSkelMeshInstance::GetMaterial);
	int TexIndex = 1000000;
	if (PolyFlags) *PolyFlags = 0;
	if (Index < pMesh->Materials.Num())
	{
		const FMeshMaterial &M = pMesh->Materials[Index];
		TexIndex  = M.TextureIndex;
		if (PolyFlags) *PolyFlags = M.PolyFlags;
	}
	// it is possible, that Textures array is empty (mesh textured by script)
	return (TexIndex < pMesh->Textures.Num()) ? MATERIAL_CAST(pMesh->Textures[TexIndex]) : NULL;
	unguard;
}

void CSkelMeshInstance::SetMaterial(int Index)
{
	guard(CSkelMeshInstance::SetMaterial);
	int PolyFlags;
	UUnrealMaterial *Mat = GetMaterial(Index, &PolyFlags);
	CMeshInstance::SetMaterial(Mat, Index, PolyFlags);
	unguard;
}


/*-----------------------------------------------------------------------------
	Create/destroy
-----------------------------------------------------------------------------*/

CSkelMeshInstance::CSkelMeshInstance()
:	LodNum(-1)
,	RotationMode(EARO_AnimSet)
,	LastLodNum(-2)				// differs from LodNum and from all other values
,	MaxAnimChannel(-1)
,	Animation(NULL)
,	DataBlock(NULL)
,	BoneData(NULL)
,	Verts(NULL)
,	Skinned(NULL)
,	NumWedges(0)
,	Sections(NULL)
,	NumSections(0)
,	Indices(NULL)
,	InfColors(NULL)
,	ShowSkel(0)
,	ShowLabels(false)
,	ShowAttach(false)
,	ShowInfluences(false)
{
	ClearSkelAnims();
}


CSkelMeshInstance::~CSkelMeshInstance()
{
	if (DataBlock) appFree(DataBlock);
	if (InfColors) delete InfColors;
}


void CSkelMeshInstance::ClearSkelAnims()
{
	// init 1st animation channel with default pose
	for (int i = 0; i < MAX_SKELANIMCHANNELS; i++)
	{
		Channels[i].AnimIndex1     = ANIM_UNASSIGNED;
		Channels[i].AnimIndex2     = ANIM_UNASSIGNED;
		Channels[i].SecondaryBlend = 0;
		Channels[i].BlendAlpha     = 1;
		Channels[i].RootBone       = 0;
	}
}


/* Iterate bone (sub)tree and do following:
 *	- find all direct childs of bone 'Index', check sort order; bones should be
 *	  sorted in hierarchy order (1st child and its children first, other childs next)
 *	  Example of tree:
 *		Bone1
 *		+- Bone11
 *		|  +- Bone111
 *		+- Bone12
 *		|  +- Bone121
 *		|  +- Bone122
 *		+- Bone13
 *	  Sorted order: Bone1, Bone11, Bone111, Bone12, Bone121, Bone122, Bone13
 *	  Note: it seems, Unreal already have sorted bone list (assumed in other code,
 *	  verified by 'assert(currIndex == Index)')
 *	- compute subtree sizes ('Sizes' array)
 *	- compute bone hierarchy depth ('Depth' array, for debugging only)
 */
static int CheckBoneTree(const TArray<FMeshBone> &Bones, int Index,
	int *Sizes, int *Depth, int &numIndices, int maxIndices)
{
	assert(numIndices < maxIndices);
	static int depth = 0;
	// remember curerent index, increment for recustion
	int currIndex = numIndices++;
	// find children of Bone[Index]
	int treeSize = 0;
	for (int i = Index + 1; i < Bones.Num(); i++)
		if (Bones[i].ParentIndex == Index)
		{
			// recurse
			depth++;
			treeSize += CheckBoneTree(Bones, i, Sizes, Depth, numIndices, maxIndices);
			depth--;
		}
	// store gathered information
//	assert(currIndex == Index);		//??
	if (currIndex != Index) appNotify("Strange skeleton, check childs of bone %d", Index);
	Sizes[currIndex] = treeSize;
	Depth[currIndex] = depth;
	return treeSize + 1;
}


static void BuildWedges(
	CSkelMeshVertex *Wedges,
	int MaxWedges,			// passed for validation only
	const TArray<FVector> &MeshPoints,
	const TArray<FMeshWedge> &MeshWedges,
	const TArray<FVertInfluences> &VertInfluences)
{
	guard(BuildWedges);

	struct CVertInfo
	{
		int		NumInfs;		// may be higher than NUM_INFLUENCES
		int		Bone[NUM_INFLUENCES];
		float	Weight[NUM_INFLUENCES];
	};

	int i, j;

	assert(MeshWedges.Num() <= MaxWedges);	// MaxWedges is a size of Wedges[] array

	CVertInfo *Verts = new CVertInfo[MeshPoints.Num()];
	memset(Verts, 0, MeshPoints.Num() * sizeof(CVertInfo));

	// collect influences per vertex
	for (i = 0; i < VertInfluences.Num(); i++)
	{
		const FVertInfluences &Inf  = VertInfluences[i];
		CVertInfo &V = Verts[Inf.PointIndex];
		int NumInfs = V.NumInfs++;
		int idx = NumInfs;
		if (NumInfs >= NUM_INFLUENCES)
		{
			// overflow
			// find smallest weight smaller than current
			float w = Inf.Weight;
			idx = -1;
			for (j = 0; j < NUM_INFLUENCES; j++)
			{
				if (V.Weight[j] < w)
				{
					w = V.Weight[j];
					idx = j;
					// continue - may be other weight will be even smaller
				}
			}
			if (idx < 0) continue;	// this weight is smaller than other
		}
		// add influence
		V.Bone[idx]   = Inf.BoneIndex;
		V.Weight[idx] = Inf.Weight;
	}

	// normalize influences
	for (i = 0; i < MeshPoints.Num(); i++)
	{
		CVertInfo &V = Verts[i];
		if (V.NumInfs <= NUM_INFLUENCES) continue;	// no normalization is required
		float s = 0;
		for (j = 0; j < NUM_INFLUENCES; j++)		// count sum
			s += V.Weight[j];
		s = 1.0f / s;
		for (j = 0; j < NUM_INFLUENCES; j++)		// adjust weights
			V.Weight[j] *= s;
	}

	// create wedges
	for (i = 0; i < MeshWedges.Num(); i++)
	{
		const FMeshWedge &SW = MeshWedges[i];
		CSkelMeshVertex  &DW = Wedges[i];
		DW.Position = CVT(MeshPoints[SW.iVertex]);
		DW.UV[0].U  = SW.TexUV.U;
		DW.UV[0].V  = SW.TexUV.V;
		// DW.Normal and DW.Tangent are unset
		// setup Bone[] and Weight[]
		const CVertInfo &V = Verts[SW.iVertex];
		for (j = 0; j < V.NumInfs; j++)
		{
			DW.Bone[j]   = V.Bone[j];
			DW.Weight[j] = V.Weight[j];
		}
		for (/* continue */; j < NUM_INFLUENCES; j++)	// place end marker and zero weight
		{
			DW.Bone[j]   = -1;
			DW.Weight[j] = 0.0f;
		}
	}

	delete Verts;

	unguard;
}


static void BuildNormals(CSkelMeshVertex *Wedges, int NumPoints, const TArray<FMeshWedge> &MeshWedges, const TArray<VTriangle> &MeshFaces)
{
	guard(BuildNormals);

//!!	if (HasTangents) return;

	CVec3 *tmpNorm;
	tmpNorm = new CVec3[NumPoints];

	// 1st pass - build notmals
	int i, j;
	for (i = 0; i < MeshFaces.Num(); i++)
	{
		const VTriangle &Face = MeshFaces[i];
		const CSkelMeshVertex *W[3];
		for (j = 0; j < 3; j++) W[j] = &Wedges[Face.WedgeIndex[j]];

		// compute edges
		CVec3 D[3];				// 0->1, 1->2, 2->0
		VectorSubtract(W[1]->Position, W[0]->Position, D[0]);
		VectorSubtract(W[2]->Position, W[1]->Position, D[1]);
		VectorSubtract(W[0]->Position, W[2]->Position, D[2]);
		// compute normal
		CVec3 norm;
		cross(D[1], D[0], norm);
		norm.Normalize();
		// compute angles
		for (j = 0; j < 3; j++) D[j].Normalize();
		float angle[3];
		angle[0] = acos(-dot(D[0], D[2]));
		angle[1] = acos(-dot(D[0], D[1]));
		angle[2] = acos(-dot(D[1], D[2]));
		// add normals for triangle verts
		for (j = 0; j < 3; j++)
			VectorMA(tmpNorm[MeshWedges[Face.WedgeIndex[j]].iVertex], angle[j], norm);
	}

	// normalize normals ...
	for (i = 0; i < NumPoints; i++)
		tmpNorm[i].Normalize();

	// ... then place normals to Wedges
	for (i = 0; i < MeshWedges.Num(); i++)
	{
		const FMeshWedge &SW = MeshWedges[i];
		CSkelMeshVertex  &DW = Wedges[i];
		DW.Normal = tmpNorm[SW.iVertex];
	}

	// 2nd pass - build tangents
	for (i = 0; i < MeshFaces.Num(); i++)
	{
		const VTriangle &Face = MeshFaces[i];
		CSkelMeshVertex *V[3];
		for (j = 0; j < 3; j++) V[j] = &Wedges[Face.WedgeIndex[j]];

		// compute tangent
		CVecT tang;
		float U0 = V[0]->UV[0].U;
		float V0 = V[0]->UV[0].V;
		float U1 = V[1]->UV[0].U;
		float V1 = V[1]->UV[0].V;
		float U2 = V[2]->UV[0].U;
		float V2 = V[2]->UV[0].V;

		if (V0 == V2)
		{
			// W[0] -> W[2] -- axis for tangent
			VectorSubtract(V[2]->Position, V[0]->Position, tang);
			// we should make tang to look in side of growing U
			if (U2 < U0) tang.Negate();
		}
		else
		{
			float pos = (V1 - V0) / (V2 - V0);			// fraction, where W[1] is placed betwen W[0] and W[2] (may be < 0 or > 1)
			CVecT tmp;
			Lerp(V[0]->Position, V[2]->Position, pos, tmp);
			VectorSubtract(tmp, V[1]->Position, tang);
			// tang.V == W[1].V; but tang.U may be greater or smaller than W[1].U
			// we should make tang to look in side of growing U
			float tU = Lerp(U0, U2, pos);
			if (tU < U1) tang.Negate();
		}
		// now, tang is on triangle plane
		// now we should place tangent orthogonal to normal, then normalize vector
		float binormalScale = 1.0f;
		for (j = 0; j < 3; j++)
		{
			CSkelMeshVertex &DW = *V[j];
			const CVecT &norm = DW.Normal;
			float pos = dot(norm, tang);
			CVecT tang2;
			VectorMA(tang, -pos, norm, tang2);
			tang2.Normalize();
			DW.Tangent = tang2;
			cross(DW.Normal, DW.Tangent, tang2);
			tang2.Normalize();
			if (j == 0)		// do this only once for triangle
			{
				// check binormal sign
				// find two points with different V
				int W1 = 0;
				int W2 = (V1 != V0) ? 1 : 2;
				// check projections of these points to binormal
				float p1 = dot(V[W1]->Position, tang2);
				float p2 = dot(V[W2]->Position, tang2);
				if ((p1 - p2) * (V[W1]->UV[0].V - V[W2]->UV[0].V) < 0)
					binormalScale = -1.0f;
			}
			tang2.Scale(binormalScale);
			DW.Binormal = tang2;
		}
	}

//!!	HasTangents = true;

	delete tmpNorm;

	unguard;
}


// Build indices for base mesh
//!! Section.Material is not set
static void BuildIndices(CSkelMeshSection *Sections, int NumSections, const TArray<VTriangle> &MeshFaces, int *Indices)
{
	guard(BuildIndices);

	memset(Sections, 0, NumSections * sizeof(CSkelMeshSection));

	int i;

	for (int pass = 0; pass < 2; pass++)
	{
		int NumIndices = 0;
		for (i = 0; i < NumSections; i++)
		{
			if (pass == 1)
			{
				int SecIndices = Sections[i].NumFaces * 3;
				Sections[i].FirstIndex = NumIndices;
				NumIndices += SecIndices;
			}
			Sections[i].NumFaces = 0;
		}

		for (i = 0; i < MeshFaces.Num(); i++)
		{
			const VTriangle &Face = MeshFaces[i];
			int MatIndex = Face.MatIndex;
			CSkelMeshSection &Sec  = Sections[MatIndex];
			assert(MatIndex < NumSections);

			if (pass == 1)	// on 1st pass count NumFaces, on 2nd pass fill indices
			{
				int *idx = Indices + Sec.FirstIndex + Sec.NumFaces * 3;
				for (int j = 0; j < 3; j++)
					*idx++ = Face.WedgeIndex[j];
			}

			Sec.NumFaces++;
		}
	}

	unguard;
}


// Build indices for LOD mesh
//!! Section.Material is not set
static void BuildIndicesLod(CSkelMeshSection *Sections, int NumSections, const FStaticLODModel *lod, int *Indices)
{
	guard(BuildIndicesLod);

	memset(Sections, 0, NumSections * sizeof(CSkelMeshSection));

	int i;

	for (int pass = 0; pass < 2; pass++)
	{
		int NumIndices = 0;
		for (i = 0; i < NumSections; i++)
		{
			if (pass == 1)
			{
				int SecIndices = Sections[i].NumFaces * 3;
				Sections[i].FirstIndex = NumIndices;
				NumIndices += SecIndices;
			}
			Sections[i].NumFaces = 0;
		}

		int s;

		// smooth sections (influence count >= 2)
		for (s = 0; s < lod->SmoothSections.Num(); s++)
		{
			const FSkelMeshSection &ms = lod->SmoothSections[s];
			int MatIndex = ms.MaterialIndex;
			CSkelMeshSection &Sec  = Sections[MatIndex];
			assert(MatIndex < NumSections);

			if (pass == 1)
			{
				int *idx = Indices + Sec.FirstIndex + Sec.NumFaces * 3;
				for (i = 0; i < ms.NumFaces; i++)
				{
					const FMeshFace &F = lod->Faces[ms.FirstFace + i];
					//?? ignore F.MaterialIndex - may be any
//					assert(F.MaterialIndex == ms.MaterialIndex);
					for (int j = 0; j < 3; j++)
						*idx++ = F.iWedge[j];
				}
			}

			Sec.NumFaces += ms.NumFaces;
		}
		// rigid sections (influence count == 1)
		for (s = 0; s < lod->RigidSections.Num(); s++)
		{
			const FSkelMeshSection &ms = lod->RigidSections[s];
			int MatIndex = ms.MaterialIndex;
			CSkelMeshSection &Sec  = Sections[MatIndex];
			assert(MatIndex < NumSections);

			if (pass == 1)
			{
				int *idx = Indices + Sec.FirstIndex + Sec.NumFaces * 3;
				for (i = 0; i < ms.NumFaces; i++)
				{
					for (int j = 0; j < 3; j++)
						*idx++ = lod->RigidIndices.Indices[(ms.FirstFace + i) * 3 + j];
				}
			}

			Sec.NumFaces += ms.NumFaces;
		}
	}

	unguard;
}


void CSkelMeshInstance::SetMesh(const USkeletalMesh *Mesh)
{
	guard(CSkelMeshInstance::SetMesh);

	int i;

	pMesh = Mesh;

	// orientation

	SetAxis(Mesh->RotOrigin, BaseTransform.axis);
	BaseTransform.origin[0] = Mesh->MeshOrigin.X * Mesh->MeshScale.X;
	BaseTransform.origin[1] = Mesh->MeshOrigin.Y * Mesh->MeshScale.Y;
	BaseTransform.origin[2] = Mesh->MeshOrigin.Z * Mesh->MeshScale.Z;

	BaseTransformScaled.axis = BaseTransform.axis;
	CVec3 tmp;
	tmp[0] = 1.0f / Mesh->MeshScale.X;
	tmp[1] = 1.0f / Mesh->MeshScale.Y;
	tmp[2] = 1.0f / Mesh->MeshScale.Z;
	BaseTransformScaled.axis.PrescaleSource(tmp);
	BaseTransformScaled.origin = CVT(Mesh->MeshOrigin);

	// skeleton

	int NumBones  = Mesh->RefSkeleton.Num();
	int NumVerts  = Mesh->Points.Num();
	NumWedges = Mesh->Wedges.Num();

	// count materials (may ge greater than Mesh->Materials.Num())
	NumSections = Mesh->Materials.Num() - 1;
	for (i = 0; i < Mesh->Triangles.Num(); i++)
	{
		int m = Mesh->Triangles[i].MatIndex;
		if (m > NumSections) NumSections = m;
	}
	NumSections++;		// before this, NumMaterials = max index of used material

	// free old data
	if (DataBlock) appFree(DataBlock);
	if (InfColors)
	{
		delete InfColors;
		InfColors = NULL;
	}
	// allocate some arrays in a single block
	int DataSize = sizeof(CMeshBoneData) * NumBones +
				   sizeof(CSkelMeshVertex) * NumWedges +		//!! outside
				   sizeof(CSkinVert) * NumWedges +
				   sizeof(CSkelMeshSection) * NumSections +		//!! outside
				   sizeof(int) * Mesh->Triangles.Num() * 3;		//!! outside
	DataBlock = appMalloc(DataSize, 16);
	BoneData  = (CMeshBoneData*)DataBlock;
	Verts     = (CSkelMeshVertex*) (BoneData + NumBones);
	Skinned   = (CSkinVert*)    (Verts    + NumWedges);
	Sections  = (CSkelMeshSection*) (Skinned  + NumWedges);
	Indices   = (int*)          (Sections + NumSections);

	LastLodNum = -2;

	CMeshBoneData *data;
	for (i = 0, data = BoneData; i < NumBones; i++, data++)
	{
		const FMeshBone &B = Mesh->RefSkeleton[i];
		// NOTE: assumed, that parent bones goes first
		assert(B.ParentIndex <= i);

		// reset animation bone map (will be set by SetAnim())
		data->BoneMap = INDEX_NONE;

		// compute reference bone coords
		CVec3 BP;
		CQuat BO;
		// get default pose
		BP = CVT(B.BonePos.Position);
		BO = CVT(B.BonePos.Orientation);
		if (!i) BO.Conjugate();

		CCoords &BC = data->RefCoords;
		BC.origin = BP;
		BO.ToAxis(BC.axis);
		// move bone position to global coordinate space
		if (i)	// do not rotate root bone
			BoneData[B.ParentIndex].RefCoords.UnTransformCoords(BC, BC);
		// store inverted transformation too
		InvertCoords(data->RefCoords, data->RefCoordsInv);
#if 0
	//!!
if (i == 32 || i == 34)
{
	appNotify("Bone %d (%8.3f %8.3f %8.3f) - (%8.3f %8.3f %8.3f %8.3f)", i, VECTOR_ARG(BP), QUAT_ARG(BO));
#define C data->RefCoords
	appNotify("REF   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	appNotify("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	appNotify("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	appNotify("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->RefCoordsInv
	appNotify("REFIN : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	appNotify("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	appNotify("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	appNotify("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
}
#endif
		// initialize skeleton configuration
		data->Scale = 1.0f;			// default bone scale
	}

	// check bones tree
	if (NumBones)
	{
		//!! should be done in USkeletalMesh
		guard(VerifySkeleton);
		int treeSizes[MAX_MESHBONES], depth[MAX_MESHBONES];
		int numIndices = 0;
		CheckBoneTree(Mesh->RefSkeleton, 0, treeSizes, depth, numIndices, MAX_MESHBONES);
		assert(numIndices == NumBones);
		for (i = 0; i < numIndices; i++)
			BoneData[i].SubtreeSize = treeSizes[i];	// remember subtree size
		unguard;
	}

	PlayAnim(NULL);

	unguard;
}


void CSkelMeshInstance::SetAnim(const CAnimSet *Anim)
{
	if (!pMesh) return;		// mesh is not set yet
	const USkeletalMesh *Mesh = static_cast<const USkeletalMesh*>(pMesh);

	Animation = Anim;

	int i;
	CMeshBoneData *data;
	for (i = 0, data = BoneData; i < Mesh->RefSkeleton.Num(); i++, data++)
	{
		const FMeshBone &B = Mesh->RefSkeleton[i];

		// find reference bone in animation track
		data->BoneMap = INDEX_NONE;		// in a case when bone has no corresponding animation track
		if (Animation)
		{
			for (int j = 0; j < Animation->TrackBoneNames.Num(); j++)
				if (!stricmp(B.Name, Animation->TrackBoneNames[j]))
				{
					data->BoneMap = j;
					break;
				}
		}
	}

	ClearSkelAnims();
	PlayAnim(NULL);
}


void CSkelMeshInstance::DumpBones()
{
#if 1
	const USkeletalMesh *Mesh = static_cast<const USkeletalMesh*>(pMesh);
	int treeSizes[MAX_MESHBONES], depth[MAX_MESHBONES];
	int numIndices = 0;
	CheckBoneTree(Mesh->RefSkeleton, 0, treeSizes, depth, numIndices, MAX_MESHBONES);
	//?? dump tree; separate function (requires depth information)
	for (int i = 0; i < numIndices; i++)
	{
		const FMeshBone &B = Mesh->RefSkeleton[i];
		int parent = B.ParentIndex;
		appPrintf("bone#%2d (parent %2d); tree size: %2d   ", i, parent, treeSizes[i]);
#if 1
		for (int j = 0; j < depth[i]; j++)
		{
	#if 0
			// simple picture
			appPrintf("|  ");
	#else
			// graph-like picture
			bool found = false;
			for (int n = i+1; n < numIndices; n++)
			{
				if (depth[n] >  j+1) continue;
				if (depth[n] == j+1) found = true;
				break;
			}
		#if _WIN32
			// pseudographics
			if (j == depth[i]-1)
				appPrintf(found ? "\xC3\xC4\xC4" : "\xC0\xC4\xC4");	// [+--] : [\--]
			else
                appPrintf(found ? "\xB3  " : "   ");				// [|  ] : [   ]
		#else
			// ASCII
			if (j == depth[i]-1)
				appPrintf(found ? "+--" : "\\--");
			else
				appPrintf(found ? "|  " : "   ");
        #endif
	#endif
		}
		appPrintf("%s\n", *B.Name);
#else
		appPrintf("%s {%g %g %g} {%g %g %g %g}\n", *B.Name, FVECTOR_ARG(B.BonePos.Position), FQUAT_ARG(B.BonePos.Orientation));
#endif
	}
#endif
}


/*-----------------------------------------------------------------------------
	Miscellaneous
-----------------------------------------------------------------------------*/

int CSkelMeshInstance::FindBone(const char *BoneName) const
{
	for (int i = 0; i < pMesh->RefSkeleton.Num(); i++)
		if (!strcmp(pMesh->RefSkeleton[i].Name, BoneName))
			return i;
	return INDEX_NONE;
}


int CSkelMeshInstance::FindAnim(const char *AnimName) const
{
	if (!Animation || !AnimName)
		return INDEX_NONE;
	for (int i = 0; i < Animation->Sequences.Num(); i++)
		if (!strcmp(Animation->Sequences[i].Name, AnimName))
			return i;
	return INDEX_NONE;
}


void CSkelMeshInstance::SetBoneScale(const char *BoneName, float scale)
{
	int BoneIndex = FindBone(BoneName);
	if (BoneIndex == INDEX_NONE) return;
	BoneData[BoneIndex].Scale = scale;
}


/*-----------------------------------------------------------------------------
	Skeletal animation itself
-----------------------------------------------------------------------------*/

#if SHOW_BONE_UPDATES
static int BoneUpdateCounts[MAX_MESHBONES];
#endif

void CSkelMeshInstance::UpdateSkeleton()
{
	guard(CSkelMeshInstance::UpdateSkeleton);

	// process all animation channels
	assert(MaxAnimChannel < MAX_SKELANIMCHANNELS);
	int Stage;
	CAnimChan *Chn;
#if SHOW_BONE_UPDATES
	memset(BoneUpdateCounts, 0, sizeof(BoneUpdateCounts));
#endif
	for (Stage = 0, Chn = Channels; Stage <= MaxAnimChannel; Stage++, Chn++)
	{
		if (Stage > 0 && (Chn->AnimIndex1 == ANIM_UNASSIGNED || Chn->BlendAlpha <= 0))
			continue;

		const CAnimSequence *AnimSeq1 = NULL;
		const CAnimSequence *AnimSeq2 = NULL;
		float Time2;
		if (Chn->AnimIndex1 >= 0)		// not INDEX_NONE or ANIM_UNASSIGNED
		{
			AnimSeq1 = &Animation->Sequences[Chn->AnimIndex1];
			if (Chn->AnimIndex2 >= 0 && Chn->SecondaryBlend)
			{
				AnimSeq2 = &Animation->Sequences[Chn->AnimIndex2];
				// compute time for secondary channel; always in sync with primary channel
				Time2 = Chn->Time / AnimSeq1->NumFrames * AnimSeq2->NumFrames;
			}
		}

		// compute bone range, affected by specified animation bone
		int firstBone = Chn->RootBone;
		int lastBone  = firstBone + BoneData[firstBone].SubtreeSize;
		assert(lastBone < pMesh->RefSkeleton.Num());

		int i;
		CMeshBoneData *data;
		for (i = firstBone, data = BoneData + firstBone; i <= lastBone; i++, data++)
		{
			if (Stage < data->FirstChannel)
			{
				// this bone position will be overrided in following channel(s); all
				// subhierarchy bones should be overrided too; skip whole subtree
				int skip = data->SubtreeSize;
				// note: 'skip' equals to subtree size; current bone is excluded - it
				// will be skipped by 'for' operator (after 'continue')
				i    += skip;
				data += skip;
				continue;
			}

			CVec3 BP;
			CQuat BO;
			const VJointPos &JP = pMesh->RefSkeleton[i].BonePos;
			BP = CVT(JP.Position);			// default position - from bind pose
			BO = CVT(JP.Orientation);		// ...

			int BoneIndex = data->BoneMap;

			// compute bone orientation
			if (AnimSeq1 && BoneIndex != INDEX_NONE)
			{
				// get bone position from track
				if (!AnimSeq2 || Chn->SecondaryBlend != 1.0f)
				{
					AnimSeq1->Tracks[BoneIndex].GetBonePosition(Chn->Time, AnimSeq1->NumFrames, Chn->Looped, BP, BO);
//const char *bname = *pMesh->RefSkeleton[i].Name;
//CQuat BOO = BO;
//if (!strcmp(bname, "b_MF_UpperArm_L")) { BO.Set(-0.225, -0.387, -0.310,  0.839); }
#if SHOW_ANIM
//if (i == 6 || i == 8 || i == 10 || i == 11 || i == 29)	//??
					DrawTextLeft("Bone (%s) : P{ %8.3f %8.3f %8.3f }  Q{ %6.3f %6.3f %6.3f %6.3f }",
						*pMesh->RefSkeleton[i].Name, VECTOR_ARG(BP), QUAT_ARG(BO));
//if (!strcmp(bname, "b_MF_UpperArm_L")) DrawTextLeft("%g %g %g %g [%g %g]", BO.x-BOO.x,BO.y-BOO.y,BO.z-BOO.z,BO.w-BOO.w, BO.w, BOO.w);
#endif
//BO.Normalize();
#if SHOW_BONE_UPDATES
					BoneUpdateCounts[i]++;
#endif
				}
				// blend secondary animation
				if (AnimSeq2)
				{
					CVec3 BP2;
					CQuat BO2;
					BP2 = CVT(JP.Position);		// default position - from bind pose
					BO2 = CVT(JP.Orientation);	// ...
					AnimSeq2->Tracks[BoneIndex].GetBonePosition(Time2, AnimSeq2->NumFrames, Chn->Looped, BP2, BO2);
					if (Chn->SecondaryBlend == 1.0f)
					{
						BO = BO2;
						BP = BP2;
					}
					else
					{
						Lerp (BP, BP2, Chn->SecondaryBlend, BP);
						Slerp(BO, BO2, Chn->SecondaryBlend, BO);
					}
#if SHOW_BONE_UPDATES
					BoneUpdateCounts[i]++;
#endif
				}
				// process AnimRotationOnly
				if (!Animation->ShouldAnimateTranslation(BoneIndex, RotationMode))
					BP = CVT(JP.Position);
			}
			else
			{
				// get default bone position
//				BP = CVT(JP.Position); -- already set above
//				BO = CVT(JP.Orientation);
#if SHOW_ANIM
				DrawTextLeft(S_YELLOW"Bone (%s) : P{ %8.3f %8.3f %8.3f }  Q{ %6.3f %6.3f %6.3f %6.3f }",
					*pMesh->RefSkeleton[i].Name, VECTOR_ARG(BP), QUAT_ARG(BO));
#endif
			}
			if (!i) BO.Conjugate();

			// tweening
			if (Chn->TweenTime > 0)
			{
				// interpolate orientation using AnimTweenStep
				// current orientation -> {BP,BO}
				Lerp (data->Pos,  BP, Chn->TweenStep, BP);
				Slerp(data->Quat, BO, Chn->TweenStep, BO);
			}
			// blending with previous channels
			if (Chn->BlendAlpha < 1.0f)
			{
				Lerp (data->Pos,  BP, Chn->BlendAlpha, BP);
				Slerp(data->Quat, BO, Chn->BlendAlpha, BO);
			}

			data->Quat = BO;
			data->Pos  = BP;
		}
	}

	// transform bones using skeleton hierarchy
	int i;
	CMeshBoneData *data;
	for (i = 0, data = BoneData; i < pMesh->RefSkeleton.Num(); i++, data++)
	{
		CCoords &BC = data->Coords;
		BC.origin = data->Pos;
		data->Quat.ToAxis(BC.axis);

		// move bone position to global coordinate space
		if (!i)
		{
			// root bone - use BaseTransform
			// can use inverted BaseTransformScaled to avoid 'slow' operation
			BaseTransformScaled.TransformCoordsSlow(BC, BC);
		}
		else
		{
			// other bones - rotate around parent bone
			BoneData[pMesh->RefSkeleton[i].ParentIndex].Coords.UnTransformCoords(BC, BC);
		}
		// deform skeleton according to external settings
		if (data->Scale != 1.0f)
		{
			BC.axis[0].Scale(data->Scale);
			BC.axis[1].Scale(data->Scale);
			BC.axis[2].Scale(data->Scale);
		}
		// compute transformation of world-space model vertices from reference
		// pose to desired pose
		BC.UnTransformCoords(data->RefCoordsInv, data->Transform);
#if USE_SSE
		data->Transform4.Set(data->Transform);
#endif
#if 0
//!!
if (i == 32 || i == 34)
{
#define C BC
	DrawTextLeft("[%2d] : o=%8.3f %8.3f %8.3f", i, VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->Transform
	DrawTextLeft("TRN   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
#define C data->RefCoordsInv
	DrawTextLeft("REF   : o=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.origin ));
	DrawTextLeft("        0=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[0]));
	DrawTextLeft("        1=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[1]));
	DrawTextLeft("        2=%8.3f %8.3f %8.3f",    VECTOR_ARG(C.axis[2]));
#undef C
}
#endif
	}
	unguard;
}


void CSkelMeshInstance::UpdateAnimation(float TimeDelta)
{
	if (!pMesh->RefSkeleton.Num()) return;	// just in case

	// prepare bone-to-channel map
	//?? optimize: update when animation changed only
	for (int i = 0; i < pMesh->RefSkeleton.Num(); i++)
		BoneData[i].FirstChannel = 0;

	assert(MaxAnimChannel < MAX_SKELANIMCHANNELS);
	int Stage;
	CAnimChan *Chn;
	for (Stage = 0, Chn = Channels; Stage <= MaxAnimChannel; Stage++, Chn++)
	{
		if (Stage > 0 && Chn->AnimIndex1 == ANIM_UNASSIGNED)
			continue;
		// update tweening
		if (Chn->TweenTime)
		{
			Chn->TweenStep = TimeDelta / Chn->TweenTime;
			Chn->TweenTime -= TimeDelta;
			if (Chn->TweenTime < 0)
			{
				// stop tweening, start animation
				TimeDelta = -Chn->TweenTime;
				Chn->TweenTime = 0;
			}
			assert(Chn->Time == 0);
		}
		// note: TweenTime may be changed now, check again
		if (!Chn->TweenTime && Chn->AnimIndex1 >= 0)
		{
			// update animation time
			const CAnimSequence *Seq1 = &Animation->Sequences[Chn->AnimIndex1];
			const CAnimSequence *Seq2 = (Chn->AnimIndex2 >= 0 && Chn->SecondaryBlend)
				? &Animation->Sequences[Chn->AnimIndex2]
				: NULL;

			float Rate1 = Chn->Rate * Seq1->Rate;
			if (Seq2)
			{
				// if blending 2 channels, should adjust animation rate
				Rate1 = Lerp(Seq1->Rate / Seq1->NumFrames, Seq2->Rate / Seq2->NumFrames, Chn->SecondaryBlend)
					* Seq1->NumFrames;
			}
			Chn->Time += TimeDelta * Rate1;

			if (Chn->Looped)
			{
				if (Chn->Time >= Seq1->NumFrames)
				{
					// wrap time
					int numSkip = appFloor(Chn->Time / Seq1->NumFrames);
					Chn->Time -= numSkip * Seq1->NumFrames;
				}
			}
			else
			{
				if (Chn->Time >= Seq1->NumFrames-1)
				{
					// clamp time
					Chn->Time = Seq1->NumFrames-1;
					if (Chn->Time < 0)
						Chn->Time = 0;
				}
			}
		}
		// assign bones to channel
		if (Chn->BlendAlpha >= 1.0f && Stage > 0) // stage 0 already set
		{
			// whole subtree will be skipped in UpdateSkeleton(), so - mark root bone only
			BoneData[Chn->RootBone].FirstChannel = Stage;
		}
	}

	UpdateSkeleton();
}


/*-----------------------------------------------------------------------------
	Animation setup
-----------------------------------------------------------------------------*/

void CSkelMeshInstance::PlayAnimInternal(const char *AnimName, float Rate, float TweenTime, int Channel, bool Looped)
{
	guard(CSkelMeshInstance::PlayAnimInternal);

	CAnimChan &Chn = GetStage(Channel);
	if (Channel > MaxAnimChannel)
		MaxAnimChannel = Channel;

	int NewAnimIndex = FindAnim(AnimName);
	if (NewAnimIndex == INDEX_NONE)
	{
		// show default pose
		Chn.AnimIndex1     = INDEX_NONE;
		Chn.AnimIndex2     = INDEX_NONE;
		Chn.Time           = 0;
		Chn.Rate           = 0;
		Chn.Looped         = false;
		Chn.TweenTime      = TweenTime;
		Chn.SecondaryBlend = 0;
		return;
	}

	Chn.Rate   = Rate;
	Chn.Looped = Looped;

	if (NewAnimIndex == Chn.AnimIndex1 && Looped)
	{
		// animation not changed, just set some flags (above)
		return;
	}

	Chn.AnimIndex1     = NewAnimIndex;
	Chn.AnimIndex2     = INDEX_NONE;
	Chn.Time           = 0;
	Chn.SecondaryBlend = 0;
	Chn.TweenTime      = TweenTime;

	unguard;
}


void CSkelMeshInstance::SetBlendParams(int Channel, float BlendAlpha, const char *BoneName)
{
	guard(CSkelMeshInstance::SetBlendParams);
	CAnimChan &Chn = GetStage(Channel);
	Chn.BlendAlpha = BlendAlpha;
	if (Channel == 0)
		Chn.BlendAlpha = 1;		// force full animation for 1st stage
	Chn.RootBone = 0;
	if (BoneName)
	{
		Chn.RootBone = FindBone(BoneName);
		if (Chn.RootBone == INDEX_NONE)	// bone not found -- ignore animation
			Chn.BlendAlpha = 0;
	}
	unguard;
}


void CSkelMeshInstance::SetBlendAlpha(int Channel, float BlendAlpha)
{
	guard(CSkelMeshInstance::SetBlendAlpha);
	GetStage(Channel).BlendAlpha = BlendAlpha;
	unguard;
}


void CSkelMeshInstance::SetSecondaryAnim(int Channel, const char *AnimName)
{
	guard(CSkelMeshInstance::SetSecondaryAnim);
	CAnimChan &Chn = GetStage(Channel);
	Chn.AnimIndex2     = FindAnim(AnimName);
	Chn.SecondaryBlend = 0;
	unguard;
}


void CSkelMeshInstance::SetSecondaryBlend(int Channel, float BlendAlpha)
{
	guard(CSkelMeshInstance::SetSecondaryBlend);
	GetStage(Channel).SecondaryBlend = BlendAlpha;
	unguard;
}


void CSkelMeshInstance::AnimStopLooping(int Channel)
{
	guard(CSkelMeshInstance::AnimStopLooping);
	GetStage(Channel).Looped = false;
	unguard;
}


void CSkelMeshInstance::FreezeAnimAt(float Time, int Channel)
{
	guard(CSkelMeshInstance::FreezeAnimAt);
	CAnimChan &Chn = GetStage(Channel);
	Chn.Time = Time;
	Chn.Rate = 0;
	unguard;
}


void CSkelMeshInstance::GetAnimParams(int Channel, const char *&AnimName,
	float &Frame, float &NumFrames, float &Rate) const
{
	guard(CSkelMeshInstance::GetAnimParams);

	const CAnimChan      &Chn  = GetStage(Channel);
	if (!Animation || Chn.AnimIndex1 < 0 || Channel > MaxAnimChannel)
	{
		AnimName  = "None";
		Frame     = 0;
		NumFrames = 0;
		Rate      = 0;
		return;
	}
	const CAnimSequence &Seq = Animation->Sequences[Chn.AnimIndex1];
	AnimName  = Seq.Name;
	Frame     = Chn.Time;
	NumFrames = Seq.NumFrames;
	Rate      = Seq.Rate * Chn.Rate;

	unguard;
}


/*-----------------------------------------------------------------------------
	Drawing
-----------------------------------------------------------------------------*/

static void GetBoneInfColor(int BoneIndex, CVec3 &Color)
{
	static float table[] = {0.1f, 0.4f, 0.7f, 1.0f};
#if 0
	Color.Set(table[BoneIndex & 3], table[(BoneIndex >> 2) & 3], table[(BoneIndex >> 4) & 3]);
#else
	#define C(x)	( (x) & 1 ) | ( ((x) >> 2) & 2 )
	Color.Set(table[C(BoneIndex)], table[C(BoneIndex >> 1)], table[C(BoneIndex >> 2)]);
	#undef C
#endif
}


void CSkelMeshInstance::DrawSkeleton(bool ShowLabels)
{
	guard(CSkelMeshInstance::DrawSkeleton);

	glDisable(GL_LIGHTING);
	BindDefaultMaterial(true);
	glLineWidth(3);
	glEnable(GL_LINE_SMOOTH);

	glBegin(GL_LINES);
	for (int i = 0; i < pMesh->RefSkeleton.Num(); i++)
	{
		const FMeshBone &B  = pMesh->RefSkeleton[i];
		const CCoords   &BC = BoneData[i].Coords;

		CVec3 v1;
		CVec3 Color;
		if (i > 0)
		{
//			Color.Set(1,1,0.3);
#if SHOW_BONE_UPDATES
			int t = BoneUpdateCounts[i];
			Color.Set(t & 1, (t >> 1) & 1, (t >> 2) & 1);
#endif
			v1 = BoneData[B.ParentIndex].Coords.origin;
		}
		else
		{
			Color.Set(1,0,1);
			v1.Zero();
		}
		if (ShowInfluences)
			GetBoneInfColor(i, Color);
		glColor3fv(Color.v);
		glVertex3fv(v1.v);
		glVertex3fv(BC.origin.v);

		if (ShowLabels)
		{
			// show bone label
			v1.Add(BC.origin);
			v1.Scale(0.5f);
			DrawText3D(v1, S_YELLOW"(%d)%s", i, *B.Name);
		}
	}
	glColor3f(1,1,1);
	glEnd();

	glLineWidth(1);
	glDisable(GL_LINE_SMOOTH);

	unguard;
}

void CSkelMeshInstance::DrawAttachments()
{
	guard(CSkelMeshInstance::DrawAttachments);

	if (!pMesh->AttachAliases.Num()) return;

	BindDefaultMaterial(true);

	glBegin(GL_LINES);
	for (int i = 0; i < pMesh->AttachAliases.Num(); i++)
	{
		const char *BoneName = pMesh->AttachBoneNames[i];
		int BoneIndex = FindBone(BoneName);
		if (BoneIndex == INDEX_NONE) continue;		// should not happen

		CCoords AC;
		BoneData[BoneIndex].Coords.UnTransformCoords(CVT(pMesh->AttachCoords[i]), AC);

		for (int j = 0; j < 3; j++)
		{
			float color[3];
			CVec3 point0, point1;

			color[0] = color[1] = color[2] = 0.1f; color[j] = 1.0f;
			point0.Set(0, 0, 0); point0[j] = 10;
			AC.UnTransformPoint(point0, point1);

			glColor3fv(color);
			glVertex3fv(AC.origin.v);
			glVertex3fv(point1.v);
		}

		// show attachment label
		CVec3 labelOrigin;
		static const CVec3 origin0 = { 4, 4, 4 };
		AC.UnTransformPoint(origin0, labelOrigin);
		DrawText3D(labelOrigin, S_GREEN"%s\n(%s)", *pMesh->AttachAliases[i], BoneName);
	}
	glColor3f(1,1,1);
	glEnd();

	unguard;
}


// Software skinning
void CSkelMeshInstance::TransformMesh()
{
	guard(CSkelMeshInstance::TransformMesh);

#if USE_SSE
	staticAssert((sizeof(CMeshBoneData) & 15) == 0, CMeshBoneData_not_mul16);
	staticAssert((sizeof(CSkelMeshVertex) & 15) == 0, CSkelMeshVertex_not_mul16);
	staticAssert((sizeof(CSkinVert) & 15) == 0, CSkinVert_not_mul16);
#endif // USE_SSE

	//?? try other tech: compute weighted sum of matrices, and then
	//?? transform vector (+ normal ?; note: normals requires normalized
	//?? transformations ?? (or normalization guaranteed by sum(weights)==1?))
	memset(Skinned, 0, sizeof(CSkinVert) * NumWedges);

	for (int i = 0; i < NumWedges; i++)
	{
		const CSkelMeshVertex &W = Verts[i];
		CSkinVert             &D = Skinned[i];

		// compute weighted transform from all influences
		// take a 1st influence
#if !USE_SSE
		CCoords transform;
		transform = BoneData[W.Bone[0]].Transform;
		transform.Scale(W.Weight[0]);
#else
		const CCoords4 &transform = BoneData[W.Bone[0]].Transform4;
		__m128 x1, x2, x3, x4, x5, x6, x7, x8;
		x1 = transform.mm[0];					// bone transform
		x2 = transform.mm[1];
		x3 = transform.mm[2];
		x4 = transform.mm[3];
		x5 = _mm_load1_ps(&W.Weight[0]);		// Weight
		x1 = _mm_mul_ps(x1, x5);				// Transform * Weight
		x2 = _mm_mul_ps(x2, x5);
		x3 = _mm_mul_ps(x3, x5);
		x4 = _mm_mul_ps(x4, x5);
#endif // USE_SSE
		// add remaining influences
		for (int j = 1; j < NUM_INFLUENCES; j++)
		{
			int iBone = W.Bone[j];
			if (iBone < 0) break;
			assert(iBone < static_cast<const USkeletalMesh*>(pMesh)->RefSkeleton.Num());	// validate bone index

			const CMeshBoneData &data = BoneData[iBone];
#if !USE_SSE
			CoordsMA(transform, W.Weight[j], data.Transform);
#else
			x5 = _mm_load1_ps(&W.Weight[j]);	// Weight
			// x1..x4 += data.Transform * Weight
			x6 = _mm_mul_ps(data.Transform4.mm[0], x5);
			x1 = _mm_add_ps(x1, x6);
			x6 = _mm_mul_ps(data.Transform4.mm[1], x5);
			x2 = _mm_add_ps(x2, x6);
			x6 = _mm_mul_ps(data.Transform4.mm[2], x5);
			x3 = _mm_add_ps(x3, x6);
			x6 = _mm_mul_ps(data.Transform4.mm[3], x5);
			x4 = _mm_add_ps(x4, x6);
#endif // USE_SSE
		}

#if !USE_SSE
		// transform vertex
		transform.UnTransformPoint(W.Pos, D.Pos);
		// transform normal
		transform.axis.UnTransformVector(W.Normal, D.Normal);
		// transform tangent
		transform.axis.UnTransformVector(W.Tangent, D.Tangent);
		// transform binormal
		transform.axis.UnTransformVector(W.Binormal, D.Binormal);
#else
		// at this point we have x1..x4 = transform matrix

#define TRANSFORM_POS(value)												\
		x5 = W.value.mm;													\
		x8 = x4;								/* Coords.origin */			\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(0,0,0,0));	/* X */			\
		x7 = _mm_mul_ps(x1, x6);											\
		x8 = _mm_add_ps(x8, x7);											\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(1,1,1,1));	/* Y */			\
		x7 = _mm_mul_ps(x2, x6);											\
		x8 = _mm_add_ps(x8, x7);											\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(2,2,2,2));	/* Z */			\
		x7 = _mm_mul_ps(x3, x6);											\
		D.value.mm = _mm_add_ps(x8, x7);

// version of code above, but without Transform.origin use
#define TRANSFORM_NORMAL(value)												\
		x5 = W.value.mm;													\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(0,0,0,0));	/* X */			\
		x8 = _mm_mul_ps(x1, x6);											\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(1,1,1,1));	/* Y */			\
		x7 = _mm_mul_ps(x2, x6);											\
		x8 = _mm_add_ps(x8, x7);											\
		x6 = _mm_shuffle_ps(x5, x5, _MM_SHUFFLE(2,2,2,2));	/* Z */			\
		x7 = _mm_mul_ps(x3, x6);											\
		D.value.mm = _mm_add_ps(x8, x7);

		TRANSFORM_POS(Position);
		TRANSFORM_NORMAL(Normal);
		TRANSFORM_NORMAL(Tangent);
		TRANSFORM_NORMAL(Binormal);
#endif // USE_SSE
	}

	unguard;
}


void CSkelMeshInstance::DrawMesh()
{
	guard(CSkelMeshInstance::DrawMesh);
	int i;

#if 0
	const USkeletalMesh *Mesh = GetMesh();
	glBegin(GL_POINTS);
	for (i = 0; i < Mesh->Points.Num(); i++)
		glVertex3fv(&Mesh->Points[i].X);
	glEnd();
	return;
#endif

#if PROFILE_MESH
	int timeBeforeTransform = appMilliseconds();
#endif
	TransformMesh();
#if PROFILE_MESH
	int timeAfterTransform = appMilliseconds();
#endif

	glEnable(GL_LIGHTING);

	// draw normal mesh

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_NORMAL_ARRAY);

	glVertexPointer(3, GL_FLOAT, sizeof(CSkinVert), &Skinned[0].Position);
	glNormalPointer(GL_FLOAT, sizeof(CSkinVert), &Skinned[0].Normal);
	glTexCoordPointer(2, GL_FLOAT, sizeof(CSkelMeshVertex), &Verts[0].UV[0].U);	//!! UV[UVIndex]

	if (ShowInfluences)
	{
		// in this mode mesh is displayed colorized instead of textured
		if (!InfColors) BuildInfColors();
		assert(InfColors);
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(3, GL_FLOAT, 0, InfColors);
		BindDefaultMaterial(true);
	}

#if SORT_BY_OPACITY
	// sort sections by material opacity
	int SectionMap[MAX_MESHMATERIALS];
	int secPlace = 0;
	for (int opacity = 0; opacity < 2; opacity++)
	{
		for (i = 0; i < NumSections; i++)
		{
			UUnrealMaterial *Mat = GetMaterial(i);
			int op = 0;			// sort value
			if (Mat && Mat->IsTranslucent()) op = 1;
			if (op == opacity) SectionMap[secPlace++] = i;
		}
	}
	assert(secPlace == NumSections);
#endif // SORT_BY_OPACITY

#if TICK_SECTIONS
	int ShowSection = (appMilliseconds() / 2000) % NumSections;
	DrawTextLeft(S_RED"Show section: %d (%d faces)", ShowSection, Sections[ShowSection].NumFaces);
#endif

	for (i = 0; i < NumSections; i++)
	{
#if TICK_SECTIONS
		if (i != ShowSection) continue;
#endif
#if SORT_BY_OPACITY
		int MaterialIndex = SectionMap[i];
#else
		int MaterialIndex = i;
#endif
		const CSkelMeshSection &Sec = Sections[MaterialIndex];
		if (!Sec.NumFaces) continue;
		// select material
		if (!ShowInfluences)
			SetMaterial(MaterialIndex);
		// check tangent space
		GLint aTangent = -1, aBinormal = -1;
		bool hasTangent = false;
		const CShader *Sh = GCurrentShader;
		if (Sh)
		{
			aTangent   = Sh->GetAttrib("tangent");
			aBinormal  = Sh->GetAttrib("binormal");
			hasTangent = (aTangent >= 0 && aBinormal >= 0);
		}
		if (hasTangent)
		{
			glEnableVertexAttribArray(aTangent);
			glEnableVertexAttribArray(aBinormal);
			glVertexAttribPointer(aTangent,  3, GL_FLOAT, GL_FALSE, sizeof(CSkinVert), &Skinned[0].Tangent);
			glVertexAttribPointer(aBinormal, 3, GL_FLOAT, GL_FALSE, sizeof(CSkinVert), &Skinned[0].Binormal);
		}
		// draw
		glDrawElements(GL_TRIANGLES, Sec.NumFaces * 3, GL_UNSIGNED_INT, Indices + Sec.FirstIndex);
		// disable tangents
		if (hasTangent)
		{
			glDisableVertexAttribArray(aTangent);
			glDisableVertexAttribArray(aBinormal);
		}
	}

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_NORMAL_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	// display debug information

	glDisable(GL_LIGHTING);
	BindDefaultMaterial(true);

#if SHOW_INFLUENCES
	const USkeletalMesh *Mesh = GetMesh();
	glBegin(GL_LINES);
	for (i = 0; i < NumWedges; i++)
	{
		const CSkelMeshVertex &W = Verts[i];
//		const FVertInfluences &Inf = Mesh->VertInfluences[i];
		for (int j = 0; j < NUM_INFLUENCES; j++)
		{
			int iBone = W.Bone[j];
			if (iBone < 0) break;
			const FMeshBone &B   = Mesh->RefSkeleton[iBone];
			const CCoords   &BC  = BoneData[iBone].Coords;
			CVec3 Color;
			GetBoneInfColor(iBone, Color);
			glColor3fv(Color.v);
			glVertex3fv(Skinned[i].Position.v);
			glVertex3fv(BC.origin.v);
		}
	}
	glEnd();
#endif // SHOW_INFLUENCES

	// draw mesh normals

	if (bShowNormals)
	{
		glBegin(GL_LINES);
		glColor3f(0.5f, 1, 0);
		for (i = 0; i < NumWedges; i++)
		{
			glVertex3fv(Skinned[i].Position.v);
			CVec3 tmp;
			VectorMA(Skinned[i].Position, 1, Skinned[i].Normal, tmp);
			glVertex3fv(tmp.v);
		}
#if SHOW_TANGENTS
		glColor3f(0, 0.5f, 1);
		for (i = 0; i < NumWedges; i++)
		{
			const CVecT &v = Skinned[i].Position;
			glVertex3fv(v.v);
			CVecT tmp;
			VectorMA(v, 1, Skinned[i].Tangent, tmp);
			glVertex3fv(tmp.v);
		}
		glColor3f(1, 0, 0.5f);
		for (i = 0; i < NumWedges; i++)
		{
			const CVecT &v = Skinned[i].Position;
			glVertex3fv(v.v);
			CVecT tmp;
			VectorMA(v, 1, Skinned[i].Binormal, tmp);
			glVertex3fv(tmp.v);
		}
#endif // SHOW_TANGENTS
		glEnd();
	}

#if PROFILE_MESH
	int timeAfterDraw = appMilliseconds();
	DrawTextRight(
		"Mesh:\n"
		"skinning:  %3d ms\n"
		"rendering: %3d ms\n",
		timeAfterTransform - timeBeforeTransform,
		timeAfterDraw - timeAfterTransform
	);
#endif // PROFILE_MESH

	unguard;
}


void CSkelMeshInstance::Draw()
{
	guard(CSkelMeshInstance::Draw);

	//?? move this part to CSkelMeshViewer; call Inst->DrawSkeleton() etc
	// show skeleton
	if (ShowSkel)
		DrawSkeleton(ShowLabels);
	if (ShowAttach)
		DrawAttachments();
	if (ShowSkel == 2) return;		// show skeleton only

	// show mesh

	if (LodNum != LastLodNum)
	{
		// LOD has been changed

		if (InfColors)
		{
			delete InfColors;
			InfColors = NULL;
		}
		// rebuild local mesh
		if (LodNum < 0)
		{
			NumWedges = pMesh->Wedges.Num();
			BuildWedges(Verts, pMesh->Wedges.Num(), pMesh->Points, pMesh->Wedges, pMesh->VertInfluences);
			BuildNormals(Verts, pMesh->Points.Num(), pMesh->Wedges, pMesh->Triangles);
			BuildIndices(Sections, NumSections, pMesh->Triangles, Indices);
		}
		else
		{
			const FStaticLODModel &Lod = pMesh->LODModels[LodNum];
			NumWedges = Lod.Wedges.Num();
			BuildWedges(Verts, pMesh->Wedges.Num(), Lod.Points, Lod.Wedges, Lod.VertInfluences);
			TArray<VTriangle> Faces;
			CopyArray(Faces, Lod.Faces);
			BuildNormals(Verts, Lod.Points.Num(), Lod.Wedges, Faces);
			BuildIndicesLod(Sections, NumSections, &Lod, Indices);
		}
		LastLodNum = LodNum;
	}

	DrawMesh();

	unguard;
}


void CSkelMeshInstance::BuildInfColors()
{
	guard(CSkelMeshInstance::BuildInfColors);

	int i;

	if (InfColors) delete InfColors;
	InfColors = new CVec3[NumWedges];

	// get colors for bones
	int NumBones = pMesh->RefSkeleton.Num();
	CVec3 BoneColors[MAX_MESHBONES];
	for (i = 0; i < NumBones; i++)
		GetBoneInfColor(i, BoneColors[i]);

	// process influences
	for (i = 0; i < NumWedges; i++)
	{
		const CSkelMeshVertex &W = Verts[i];
		for (int j = 0; j < NUM_INFLUENCES; j++)
		{
			if (W.Bone[j] < 0) break;
			VectorMA(InfColors[i], W.Weight[j], BoneColors[W.Bone[j]]);
		}
	}

	unguard;
}

#endif // RENDERING
