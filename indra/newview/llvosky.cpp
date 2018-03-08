/** 
 * @file llvosky.cpp
 * @brief LLVOSky class implementation
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 * 
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * 
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * 
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llvosky.h"

#include "llfeaturemanager.h"
#include "llviewercontrol.h"
#include "llframetimer.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "lldrawable.h"
#include "llface.h"
#include "llcubemap.h"
#include "lldrawpoolsky.h"
#include "lldrawpoolwater.h"
#include "llglheaders.h"
#include "llsky.h"
#include "llviewercamera.h"
#include "llviewertexturelist.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "pipeline.h"
#include "lldrawpoolwlsky.h"
#include "v3colorutil.h"

#include "llsettingssky.h"
#include "llenvironment.h"

#undef min
#undef max

static const S32 NUM_TILES_X = 8;
static const S32 NUM_TILES_Y = 4;
static const S32 NUM_TILES = NUM_TILES_X * NUM_TILES_Y;

// Heavenly body constants
static const F32 SUN_DISK_RADIUS	= 0.5f;
static const F32 MOON_DISK_RADIUS	= SUN_DISK_RADIUS * 0.9f;
static const F32 SUN_INTENSITY = 1e5;

// Texture coordinates:
static const LLVector2 TEX00 = LLVector2(0.f, 0.f);
static const LLVector2 TEX01 = LLVector2(0.f, 1.f);
static const LLVector2 TEX10 = LLVector2(1.f, 0.f);
static const LLVector2 TEX11 = LLVector2(1.f, 1.f);

// Exported globals
LLUUID gSunTextureID = IMG_SUN;
LLUUID gMoonTextureID = IMG_MOON;

F32 clip_side_to_horizon(const LLVector3& V0, const LLVector3& V1, const F32 cos_max_angle)
{
	const LLVector3 V = V1 - V0;
	const F32 k2 = 1.f/(cos_max_angle * cos_max_angle) - 1;
	const F32 A = V.mV[0] * V.mV[0] + V.mV[1] * V.mV[1] - k2 * V.mV[2] * V.mV[2];
	const F32 B = V0.mV[0] * V.mV[0] + V0.mV[1] * V.mV[1] - k2 * V0.mV[2] * V.mV[2];
	const F32 C = V0.mV[0] * V0.mV[0] + V0.mV[1] * V0.mV[1] - k2 * V0.mV[2] * V0.mV[2];

	if (fabs(A) < 1e-7)
	{
		return -0.1f;	// v0 is cone origin and v1 is on the surface of the cone.
	}

	const F32 det = sqrt(B*B - A*C);
	const F32 t1 = (-B - det) / A;
	const F32 t2 = (-B + det) / A;
	const F32 z1 = V0.mV[2] + t1 * V.mV[2];
	const F32 z2 = V0.mV[2] + t2 * V.mV[2];
	if (z1 * cos_max_angle < 0)
	{
		return t2;
	}
	else if (z2 * cos_max_angle < 0)
	{
		return t1;
	}
	else if ((t1 < 0) || (t1 > 1))
	{
		return t2;
	}
	else
	{
		return t1;
	}
}

// Clips quads with top and bottom sides parallel to horizon.
BOOL clip_quad_to_horizon(F32& t_left, F32& t_right, LLVector3 v_clipped[4],
						  const LLVector3 v_corner[4], const F32 cos_max_angle)
{
	t_left = clip_side_to_horizon(v_corner[1], v_corner[0], cos_max_angle);
	t_right = clip_side_to_horizon(v_corner[3], v_corner[2], cos_max_angle);

	if ((t_left >= 1) || (t_right >= 1))
	{
		return FALSE;
	}

	//const BOOL left_clip = (t_left > 0);
	//const BOOL right_clip = (t_right > 0);

	//if (!left_clip && !right_clip)
	{
		for (S32 vtx = 0; vtx < 4; ++vtx)
		{
			v_clipped[vtx]  = v_corner[vtx];
		}
	}
/*	else
	{
		v_clipped[0] = v_corner[0];
		v_clipped[1] = left_clip ? ((1 - t_left) * v_corner[1] + t_left * v_corner[0])
									: v_corner[1];
		v_clipped[2] = v_corner[2];
		v_clipped[3] = right_clip ? ((1 - t_right) * v_corner[3] + t_right * v_corner[2])
									: v_corner[3];
	}*/

	return TRUE;
}

/***************************************
		SkyTex
***************************************/

S32 LLSkyTex::sComponents = 4;
S32 LLSkyTex::sResolution = 64;
F32 LLSkyTex::sInterpVal = 0.f;
S32 LLSkyTex::sCurrent = 0;


LLSkyTex::LLSkyTex() :
	mSkyData(NULL),
	mSkyDirs(NULL)
{
}

void LLSkyTex::init()
{
	mSkyData = new LLColor4[sResolution * sResolution];
	mSkyDirs = new LLVector3[sResolution * sResolution];

	for (S32 i = 0; i < 2; ++i)
	{
		mTexture[i] = LLViewerTextureManager::getLocalTexture(FALSE);
		mTexture[i]->setAddressMode(LLTexUnit::TAM_CLAMP);
		mImageRaw[i] = new LLImageRaw(sResolution, sResolution, sComponents);
		
		initEmpty(i);
	}
}

void LLSkyTex::cleanupGL()
{
	mTexture[0] = NULL;
	mTexture[1] = NULL;
}

void LLSkyTex::restoreGL()
{
	for (S32 i = 0; i < 2; i++)
	{
		mTexture[i] = LLViewerTextureManager::getLocalTexture(FALSE);
		mTexture[i]->setAddressMode(LLTexUnit::TAM_CLAMP);
	}
}

LLSkyTex::~LLSkyTex()
{
	delete[] mSkyData;
	mSkyData = NULL;

	delete[] mSkyDirs;
	mSkyDirs = NULL;
}


void LLSkyTex::initEmpty(const S32 tex)
{
	U8* data = mImageRaw[tex]->getData();
	for (S32 i = 0; i < sResolution; ++i)
	{
		for (S32 j = 0; j < sResolution; ++j)
		{
			const S32 basic_offset = (i * sResolution + j);
			S32 offset = basic_offset * sComponents;
			data[offset] = 0;
			data[offset+1] = 0;
			data[offset+2] = 0;
			data[offset+3] = 255;

			mSkyData[basic_offset].setToBlack();
		}
	}

	createGLImage(tex);
}

void LLSkyTex::create(const F32 brightness)
{
	/// Brightness ignored for now.
	U8* data = mImageRaw[sCurrent]->getData();
	for (S32 i = 0; i < sResolution; ++i)
	{
		for (S32 j = 0; j < sResolution; ++j)
		{
			const S32 basic_offset = (i * sResolution + j);
			S32 offset = basic_offset * sComponents;
			U32* pix = (U32*)(data + offset);
			LLColor4U temp = LLColor4U(mSkyData[basic_offset]);
			*pix = temp.asRGBA();
		}
	}
	createGLImage(sCurrent);
}




void LLSkyTex::createGLImage(S32 which)
{	
	mTexture[which]->createGLTexture(0, mImageRaw[which], 0, TRUE, LLGLTexture::LOCAL);
	mTexture[which]->setAddressMode(LLTexUnit::TAM_CLAMP);
}

void LLSkyTex::bindTexture(BOOL curr)
{
	gGL.getTexUnit(0)->bind(mTexture[getWhich(curr)], true);
}

/***************************************
		Sky
***************************************/

F32	LLHeavenBody::sInterpVal = 0;

S32 LLVOSky::sResolution = LLSkyTex::getResolution();
S32 LLVOSky::sTileResX = sResolution/NUM_TILES_X;
S32 LLVOSky::sTileResY = sResolution/NUM_TILES_Y;

LLVOSky::LLVOSky(const LLUUID &id, const LLPCode pcode, LLViewerRegion *regionp)
:	LLStaticViewerObject(id, pcode, regionp, TRUE),
	mSun(SUN_DISK_RADIUS), mMoon(MOON_DISK_RADIUS),
	mBrightnessScale(1.f),
	mBrightnessScaleNew(0.f),
	mBrightnessScaleGuess(1.f),
	mWeatherChange(FALSE),
	mCloudDensity(0.2f),
	mWind(0.f),
	mForceUpdate(FALSE),
	mWorldScale(1.f),
	mBumpSunDir(0.f, 0.f, 1.f)
{
	/// WL PARAMS

	mInitialized = FALSE;
	mbCanSelect = FALSE;
	mUpdateTimer.reset();

	for (S32 i = 0; i < 6; i++)
	{
		mSkyTex[i].init();
		mShinyTex[i].init();
	}
	for (S32 i=0; i<FACE_COUNT; i++)
	{
		mFace[i] = NULL;
	}
	
	mCameraPosAgent = gAgentCamera.getCameraPositionAgent();
	mAtmHeight = ATM_HEIGHT;
	mEarthCenter = LLVector3(mCameraPosAgent.mV[0], mCameraPosAgent.mV[1], -EARTH_RADIUS);

    // *LAPRAS
    mSunDefaultPosition = LLEnvironment::instance().getCurrentSky()->getSunDirection();

	if (gSavedSettings.getBOOL("SkyOverrideSimSunPosition"))
	{

        initSunDirection(LLVector3(mSunDefaultPosition.mV[2], mSunDefaultPosition.mV[0], mSunDefaultPosition.mV[1]), LLVector3(0, 0, 0));
	}
	

	mSun.setIntensity(SUN_INTENSITY);
	mMoon.setIntensity(0.1f * SUN_INTENSITY);

	mSunTexturep = LLViewerTextureManager::getFetchedTexture(gSunTextureID, FTT_DEFAULT, TRUE, LLGLTexture::BOOST_UI);
	mSunTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);
	mMoonTexturep = LLViewerTextureManager::getFetchedTexture(gMoonTextureID, FTT_DEFAULT, TRUE, LLGLTexture::BOOST_UI);
	mMoonTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);
	mBloomTexturep = LLViewerTextureManager::getFetchedTexture(IMG_BLOOM1);
	mBloomTexturep->setNoDelete() ;
	mBloomTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);

	mHeavenlyBodyUpdated = FALSE ;

	mDrawRefl = 0;
    mInterpVal = 0.f;
}


LLVOSky::~LLVOSky()
{
	// Don't delete images - it'll get deleted by gTextureList on shutdown
	// This needs to be done for each texture

	mCubeMap = NULL;
}

void LLVOSky::init()
{
	updateDirections();

	// Initialize the cached normalized direction vectors
	for (S32 side = 0; side < 6; ++side)
	{
		for (S32 tile = 0; tile < NUM_TILES; ++tile)
		{
			initSkyTextureDirs(side, tile);
			createSkyTexture(side, tile);
		}
	}

	for (S32 i = 0; i < 6; ++i)
	{
		mSkyTex[i].create(1.0f);
		mShinyTex[i].create(1.0f);
	}

	initCubeMap();
	mInitialized = true;

	mHeavenlyBodyUpdated = FALSE ;
}

void LLVOSky::initCubeMap() 
{
	std::vector<LLPointer<LLImageRaw> > images;
	for (S32 side = 0; side < 6; side++)
	{
		images.push_back(mShinyTex[side].getImageRaw());
	}
	if (mCubeMap)
	{
		mCubeMap->init(images);
	}
	else if (gSavedSettings.getBOOL("RenderWater") && gGLManager.mHasCubeMap && LLCubeMap::sUseCubeMaps)
	{
		mCubeMap = new LLCubeMap();
		mCubeMap->init(images);
	}
	gGL.getTexUnit(0)->disable();
}


void LLVOSky::cleanupGL()
{
	S32 i;
	for (i = 0; i < 6; i++)
	{
		mSkyTex[i].cleanupGL();
	}
	if (getCubeMap())
	{
		getCubeMap()->destroyGL();
	}
}

void LLVOSky::restoreGL()
{
	S32 i;
	for (i = 0; i < 6; i++)
	{
		mSkyTex[i].restoreGL();
	}
	mSunTexturep = LLViewerTextureManager::getFetchedTexture(gSunTextureID, FTT_DEFAULT, TRUE, LLGLTexture::BOOST_UI);
	mSunTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);
	mMoonTexturep = LLViewerTextureManager::getFetchedTexture(gMoonTextureID, FTT_DEFAULT, TRUE, LLGLTexture::BOOST_UI);
	mMoonTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);
	mBloomTexturep = LLViewerTextureManager::getFetchedTexture(IMG_BLOOM1);
	mBloomTexturep->setNoDelete() ;
	mBloomTexturep->setAddressMode(LLTexUnit::TAM_CLAMP);

	updateDirections();

	if (gSavedSettings.getBOOL("RenderWater") && gGLManager.mHasCubeMap
	    && LLCubeMap::sUseCubeMaps)
	{
		LLCubeMap* cube_map = getCubeMap();

		std::vector<LLPointer<LLImageRaw> > images;
		for (S32 side = 0; side < 6; side++)
		{
			images.push_back(mShinyTex[side].getImageRaw());
		}

		if(cube_map)
		{
			cube_map->init(images);
			mForceUpdate = TRUE;
		}
	}

	if (mDrawable)
	{
		gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME, TRUE);
	}

}

void LLVOSky::initSkyTextureDirs(const S32 side, const S32 tile)
{
	S32 tile_x = tile % NUM_TILES_X;
	S32 tile_y = tile / NUM_TILES_X;

	S32 tile_x_pos = tile_x * sTileResX;
	S32 tile_y_pos = tile_y * sTileResY;

	F32 coeff[3] = {0, 0, 0};
	const S32 curr_coef = side >> 1; // 0/1 = Z axis, 2/3 = Y, 4/5 = X
	const S32 side_dir = (((side & 1) << 1) - 1);  // even = -1, odd = 1
	const S32 x_coef = (curr_coef + 1) % 3;
	const S32 y_coef = (x_coef + 1) % 3;

	coeff[curr_coef] = (F32)side_dir;

	F32 inv_res = 1.f/sResolution;
	S32 x, y;
	for (y = tile_y_pos; y < (tile_y_pos + sTileResY); ++y)
	{
		for (x = tile_x_pos; x < (tile_x_pos + sTileResX); ++x)
		{
			coeff[x_coef] = F32((x<<1) + 1) * inv_res - 1.f;
			coeff[y_coef] = F32((y<<1) + 1) * inv_res - 1.f;
			LLVector3 dir(coeff[0], coeff[1], coeff[2]);
			dir.normalize();
			mSkyTex[side].setDir(dir, x, y);
			mShinyTex[side].setDir(dir, x, y);
		}
	}
}

void LLVOSky::createSkyTexture(const S32 side, const S32 tile)
{
	S32 tile_x = tile % NUM_TILES_X;
	S32 tile_y = tile / NUM_TILES_X;

	S32 tile_x_pos = tile_x * sTileResX;
	S32 tile_y_pos = tile_y * sTileResY;

	S32 x, y;
	for (y = tile_y_pos; y < (tile_y_pos + sTileResY); ++y)
	{
		for (x = tile_x_pos; x < (tile_x_pos + sTileResX); ++x)
		{
			mSkyTex[side].setPixel(m_legacyAtmospherics.calcSkyColorInDir(mSkyTex[side].getDir(x, y)), x, y);
			mShinyTex[side].setPixel(m_legacyAtmospherics.calcSkyColorInDir(mSkyTex[side].getDir(x, y), true), x, y);
		}
	}
}

void LLVOSky::updateDirections(void)
{
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

    mSun.setColor(psky->getSunlightColor());
	mMoon.setColor(LLColor3(1.0f, 1.0f, 1.0f));

	mSun.renewDirection();
	mSun.renewColor();
	mMoon.renewDirection();
	mMoon.renewColor();

	float dp = getToSunLast() * LLVector3(0,0,1.f);
	if (dp < 0)
	{
		dp = 0;
	}

	// Since WL scales everything by 2, there should always be at least a 2:1 brightness ratio
	// between sunlight and point lights in windlight to normalize point lights.
	F32 sun_dynamic_range = llmax(gSavedSettings.getF32("RenderSunDynamicRange"), 0.0001f);
    LLEnvironment::instance().setSceneLightStrength(2.0f * (1.0f + sun_dynamic_range * dp));
}

void LLVOSky::idleUpdate(LLAgent &agent, const F64 &time)
{
}

BOOL LLVOSky::updateSky()
{
    LLSettingsSky::ptr_t psky = LLEnvironment::instance().getCurrentSky();

    LLColor4 total_ambient = psky->getTotalAmbient();

	if (mDead || !(gPipeline.hasRenderType(LLPipeline::RENDER_TYPE_SKY)))
	{
		return TRUE;
	}
	
	if (mDead)
	{
		// It's dead.  Don't update it.
		return TRUE;
	}
	if (gGLManager.mIsDisabled)
	{
		return TRUE;
	}

	static S32 next_frame = 0;
	const S32 total_no_tiles = 6 * NUM_TILES;
	const S32 cycle_frame_no = total_no_tiles + 1;

	if (mUpdateTimer.getElapsedTimeF32() > 0.025f)
	{
		mUpdateTimer.reset();
		const S32 frame = next_frame;

		++next_frame;
		next_frame = next_frame % cycle_frame_no;

		mInterpVal = (!mInitialized) ? 1 : (F32)next_frame / cycle_frame_no;
		// sInterpVal = (F32)next_frame / cycle_frame_no;
		LLSkyTex::setInterpVal( mInterpVal );
		LLHeavenBody::setInterpVal( mInterpVal );
		updateDirections();

		if (mForceUpdate || total_no_tiles == frame)
		{
		LLSkyTex::stepCurrent();
			
		const static F32 LIGHT_DIRECTION_THRESHOLD = (F32) cos(DEG_TO_RAD * 1.f);
		const static F32 COLOR_CHANGE_THRESHOLD = 0.01f;

		LLVector3 direction = mSun.getDirection();
		direction.normalize();
		const F32 dot_lighting = direction * mLastLightingDirection;

		LLColor3 delta_color;
		delta_color.setVec(mLastTotalAmbient.mV[0] - total_ambient.mV[0],
							mLastTotalAmbient.mV[1] - total_ambient.mV[1],
                            mLastTotalAmbient.mV[2] - total_ambient.mV[2]);

		if ( mForceUpdate 
				|| (((dot_lighting < LIGHT_DIRECTION_THRESHOLD)
				|| (delta_color.length() > COLOR_CHANGE_THRESHOLD)
				|| !mInitialized)
			&& !direction.isExactlyZero()))
		{
			mLastLightingDirection = direction;
            mLastTotalAmbient = total_ambient;
			mInitialized = TRUE;

			if (mCubeMap)
			{
                    if (mForceUpdate)
					{
				updateFog(LLViewerCamera::getInstance()->getFar());

				for (int side = 0; side < 6; side++) 
				{
					for (int tile = 0; tile < NUM_TILES; tile++) 
					{
						createSkyTexture(side, tile);
					}
				}

				for (int side = 0; side < 6; side++) 
				{
					LLImageRaw* raw1 = mSkyTex[side].getImageRaw(TRUE);
					LLImageRaw* raw2 = mSkyTex[side].getImageRaw(FALSE);
					raw2->copy(raw1);
					mSkyTex[side].createGLImage(mSkyTex[side].getWhich(FALSE));

					raw1 = mShinyTex[side].getImageRaw(TRUE);
					raw2 = mShinyTex[side].getImageRaw(FALSE);
					raw2->copy(raw1);
					mShinyTex[side].createGLImage(mShinyTex[side].getWhich(FALSE));
				}
				next_frame = 0;	

                // update the sky texture
			    for (S32 i = 0; i < 6; ++i)
			    {
				    mSkyTex[i].create(1.0f);
				    mShinyTex[i].create(1.0f);
			    }

                // update the environment map
			            if (mCubeMap)
			            {
				std::vector<LLPointer<LLImageRaw> > images;
				images.reserve(6);
				for (S32 side = 0; side < 6; side++)
				{
					images.push_back(mShinyTex[side].getImageRaw(TRUE));
				}
				mCubeMap->init(images);
				gGL.getTexUnit(0)->disable();
			}
					}
				}
			}

			gPipeline.markRebuild(gSky.mVOGroundp->mDrawable, LLDrawable::REBUILD_ALL, TRUE);
			mForceUpdate = FALSE;
		}
	}

	if (mDrawable.notNull() && mDrawable->getFace(0) && !mDrawable->getFace(0)->getVertexBuffer())
	{
		gPipeline.markRebuild(mDrawable, LLDrawable::REBUILD_VOLUME, TRUE);
	}

	return TRUE;
}

void LLVOSky::updateTextures()
{
	if (mSunTexturep)
	{
		mSunTexturep->addTextureStats( (F32)MAX_IMAGE_AREA );
		mMoonTexturep->addTextureStats( (F32)MAX_IMAGE_AREA );
		mBloomTexturep->addTextureStats( (F32)MAX_IMAGE_AREA );
	}
}

LLDrawable *LLVOSky::createDrawable(LLPipeline *pipeline)
{
	pipeline->allocDrawable(this);
	mDrawable->setLit(FALSE);

	LLDrawPoolSky *poolp = (LLDrawPoolSky*) gPipeline.getPool(LLDrawPool::POOL_SKY);
	poolp->setSkyTex(mSkyTex);
	mDrawable->setRenderType(LLPipeline::RENDER_TYPE_SKY);
	
	for (S32 i = 0; i < 6; ++i)
	{
		mFace[FACE_SIDE0 + i] = mDrawable->addFace(poolp, NULL);
	}

	mFace[FACE_SUN] = mDrawable->addFace(poolp, mSunTexturep);
	mFace[FACE_MOON] = mDrawable->addFace(poolp, mMoonTexturep);
	mFace[FACE_BLOOM] = mDrawable->addFace(poolp, mBloomTexturep);

	return mDrawable;
}

static LLTrace::BlockTimerStatHandle FTM_GEO_SKY("Sky Geometry");

BOOL LLVOSky::updateGeometry(LLDrawable *drawable)
{
	LL_RECORD_BLOCK_TIME(FTM_GEO_SKY);
	if (mFace[FACE_REFLECTION] == NULL)
	{
		LLDrawPoolWater *poolp = (LLDrawPoolWater*) gPipeline.getPool(LLDrawPool::POOL_WATER);
		if (gPipeline.getPool(LLDrawPool::POOL_WATER)->getVertexShaderLevel() != 0)
		{
			mFace[FACE_REFLECTION] = drawable->addFace(poolp, NULL);
		}
	}

	mCameraPosAgent = drawable->getPositionAgent();

	mEarthCenter.mV[0] = mCameraPosAgent.mV[0];
	mEarthCenter.mV[1] = mCameraPosAgent.mV[1];

	LLVector3 v_agent[8];
	for (S32 i = 0; i < 8; ++i)
	{
		F32 x_sgn = (i&1) ? 1.f : -1.f;
		F32 y_sgn = (i&2) ? 1.f : -1.f;
		F32 z_sgn = (i&4) ? 1.f : -1.f;
		v_agent[i] = HORIZON_DIST * SKY_BOX_MULT * LLVector3(x_sgn, y_sgn, z_sgn);
	}

	LLStrider<LLVector3> verticesp;
	LLStrider<LLVector3> normalsp;
	LLStrider<LLVector2> texCoordsp;
	LLStrider<U16> indicesp;
	U16 index_offset;
	LLFace *face;	

	for (S32 side = 0; side < 6; ++side)
	{
		face = mFace[FACE_SIDE0 + side]; 

		if (!face->getVertexBuffer())
		{
			face->setSize(4, 6);
			face->setGeomIndex(0);
			face->setIndicesIndex(0);
			LLVertexBuffer* buff = new LLVertexBuffer(LLDrawPoolSky::VERTEX_DATA_MASK, GL_STREAM_DRAW_ARB);
			buff->allocateBuffer(4, 6, TRUE);
			face->setVertexBuffer(buff);

			index_offset = face->getGeometry(verticesp,normalsp,texCoordsp, indicesp);
			
			S32 vtx = 0;
			S32 curr_bit = side >> 1; // 0/1 = Z axis, 2/3 = Y, 4/5 = X
			S32 side_dir = side & 1;  // even - 0, odd - 1
			S32 i_bit = (curr_bit + 2) % 3;
			S32 j_bit = (i_bit + 2) % 3;

			LLVector3 axis;
			axis.mV[curr_bit] = 1;
			face->mCenterAgent = (F32)((side_dir << 1) - 1) * axis * HORIZON_DIST;

			vtx = side_dir << curr_bit;
			*(verticesp++)  = v_agent[vtx];
			*(verticesp++)  = v_agent[vtx | 1 << j_bit];
			*(verticesp++)  = v_agent[vtx | 1 << i_bit];
			*(verticesp++)  = v_agent[vtx | 1 << i_bit | 1 << j_bit];

			*(texCoordsp++) = TEX00;
			*(texCoordsp++) = TEX01;
			*(texCoordsp++) = TEX10;
			*(texCoordsp++) = TEX11;

			// Triangles for each side
			*indicesp++ = index_offset + 0;
			*indicesp++ = index_offset + 1;
			*indicesp++ = index_offset + 3;

			*indicesp++ = index_offset + 0;
			*indicesp++ = index_offset + 3;
			*indicesp++ = index_offset + 2;

			buff->flush();
		}
	}

	const LLVector3 &look_at = LLViewerCamera::getInstance()->getAtAxis();
	LLVector3 right = look_at % LLVector3::z_axis;
	LLVector3 up = right % look_at;
	right.normalize();
	up.normalize();

	const static F32 elevation_factor = 0.0f/sResolution;
	const F32 cos_max_angle = cosHorizon(elevation_factor);
	mSun.setDraw(updateHeavenlyBodyGeometry(drawable, FACE_SUN, TRUE, mSun, cos_max_angle, up, right));
	mMoon.setDraw(updateHeavenlyBodyGeometry(drawable, FACE_MOON, FALSE, mMoon, cos_max_angle, up, right));

	const F32 water_height = gAgent.getRegion()->getWaterHeight() + 0.01f;
		// LLWorld::getInstance()->getWaterHeight() + 0.01f;
	const F32 camera_height = mCameraPosAgent.mV[2];
	const F32 height_above_water = camera_height - water_height;

	BOOL sun_flag = FALSE;

	if (mSun.isVisible())
	{
		if (mMoon.isVisible())
		{
			sun_flag = look_at * mSun.getDirection() > 0;
		}
		else
		{
			sun_flag = TRUE;
		}
	}
	
	if (height_above_water > 0)
	{
		BOOL render_ref = gPipeline.getPool(LLDrawPool::POOL_WATER)->getVertexShaderLevel() == 0;

		if (sun_flag)
		{
			setDrawRefl(0);
			if (render_ref)
			{
				updateReflectionGeometry(drawable, height_above_water, mSun);
			}
		}
		else
		{
			setDrawRefl(1);
			if (render_ref)
			{
				updateReflectionGeometry(drawable, height_above_water, mMoon);
			}
		}
	}
	else
	{
		setDrawRefl(-1);
	}

	LLPipeline::sCompiles++;
	return TRUE;
}

BOOL LLVOSky::updateHeavenlyBodyGeometry(LLDrawable *drawable, const S32 f, const BOOL is_sun,
										 LLHeavenBody& hb, const F32 cos_max_angle,
										 const LLVector3 &up, const LLVector3 &right)
{
	mHeavenlyBodyUpdated = TRUE ;

	LLStrider<LLVector3> verticesp;
	LLStrider<LLVector3> normalsp;
	LLStrider<LLVector2> texCoordsp;
	LLStrider<U16> indicesp;
	S32 index_offset;
	LLFace *facep;

	LLVector3 to_dir = hb.getDirection();

	if (!is_sun)
	{
		to_dir.mV[2] = llmax(to_dir.mV[2]+0.1f, 0.1f);
	}
	LLVector3 draw_pos = to_dir * HEAVENLY_BODY_DIST;


	LLVector3 hb_right = to_dir % LLVector3::z_axis;
	LLVector3 hb_up = hb_right % to_dir;
	hb_right.normalize();
	hb_up.normalize();

	//const static F32 cos_max_turn = sqrt(3.f) / 2; // 30 degrees
	//const F32 cos_turn_right = 1. / (llmax(cos_max_turn, hb_right * right));
	//const F32 cos_turn_up = 1. / llmax(cos_max_turn, hb_up * up);

	const F32 enlargm_factor = ( 1 - to_dir.mV[2] );
	F32 horiz_enlargement = 1 + enlargm_factor * 0.3f;
	F32 vert_enlargement = 1 + enlargm_factor * 0.2f;

	// Parameters for the water reflection
	hb.setU(HEAVENLY_BODY_FACTOR * horiz_enlargement * hb.getDiskRadius() * hb_right);
	hb.setV(HEAVENLY_BODY_FACTOR * vert_enlargement * hb.getDiskRadius() * hb_up);
	// End of parameters for the water reflection

	const LLVector3 scaled_right = HEAVENLY_BODY_DIST * hb.getU();
	const LLVector3 scaled_up = HEAVENLY_BODY_DIST * hb.getV();

	//const LLVector3 scaled_right = horiz_enlargement * HEAVENLY_BODY_SCALE * hb.getDiskRadius() * hb_right;//right;
	//const LLVector3 scaled_up = vert_enlargement * HEAVENLY_BODY_SCALE * hb.getDiskRadius() * hb_up;//up;
	LLVector3 v_clipped[4];

	hb.corner(0) = draw_pos - scaled_right + scaled_up;
	hb.corner(1) = draw_pos - scaled_right - scaled_up;
	hb.corner(2) = draw_pos + scaled_right + scaled_up;
	hb.corner(3) = draw_pos + scaled_right - scaled_up;


	F32 t_left, t_right;
	if (!clip_quad_to_horizon(t_left, t_right, v_clipped, hb.corners(), cos_max_angle))
	{
		hb.setVisible(FALSE);
		return FALSE;
	}
	hb.setVisible(TRUE);

	facep = mFace[f]; 

	if (!facep->getVertexBuffer())
	{
		facep->setSize(4, 6);	
		LLVertexBuffer* buff = new LLVertexBuffer(LLDrawPoolSky::VERTEX_DATA_MASK, GL_STREAM_DRAW_ARB);
		if (!buff->allocateBuffer(facep->getGeomCount(), facep->getIndicesCount(), TRUE))
		{
			LL_WARNS() << "Failed to allocate Vertex Buffer for vosky to "
				<< facep->getGeomCount() << " vertices and "
				<< facep->getIndicesCount() << " indices" << LL_ENDL;
		}
		facep->setGeomIndex(0);
		facep->setIndicesIndex(0);
		facep->setVertexBuffer(buff);
	}

	llassert(facep->getVertexBuffer()->getNumIndices() == 6);

	index_offset = facep->getGeometry(verticesp,normalsp,texCoordsp, indicesp);

	if (-1 == index_offset)
	{
		return TRUE;
	}

	for (S32 vtx = 0; vtx < 4; ++vtx)
	{
		hb.corner(vtx) = v_clipped[vtx];
		*(verticesp++)  = hb.corner(vtx) + mCameraPosAgent;
	}

	*(texCoordsp++) = TEX01;
	*(texCoordsp++) = TEX00;
	*(texCoordsp++) = TEX11;
	*(texCoordsp++) = TEX10;

	*indicesp++ = index_offset + 0;
	*indicesp++ = index_offset + 2;
	*indicesp++ = index_offset + 1;

	*indicesp++ = index_offset + 1;
	*indicesp++ = index_offset + 2;
	*indicesp++ = index_offset + 3;

	facep->getVertexBuffer()->flush();

	if (is_sun)
	{
		if ((t_left > 0) && (t_right > 0))
		{
			F32 t = (t_left + t_right) * 0.5f;
			mSun.setHorizonVisibility(0.5f * (1 + cos(t * F_PI)));
		}
		else
		{
			mSun.setHorizonVisibility();
		}
		updateSunHaloGeometry(drawable);
	}

	return TRUE;
}

void LLVOSky::updateSunHaloGeometry(LLDrawable *drawable )
{
#if 0
	const LLVector3* v_corner = mSun.corners();

	LLStrider<LLVector3> verticesp;
	LLStrider<LLVector3> normalsp;
	LLStrider<LLVector2> texCoordsp;
	LLStrider<U16> indicesp;
	S32 index_offset;
	LLFace *face;

	const LLVector3 right = 2 * (v_corner[2] - v_corner[0]);
	LLVector3 up = 2 * (v_corner[2] - v_corner[3]);
	up.normalize();
	F32 size = right.length();
	up = size * up;
	const LLVector3 draw_pos = 0.25 * (v_corner[0] + v_corner[1] + v_corner[2] + v_corner[3]);
	
	LLVector3 v_glow_corner[4];

	v_glow_corner[0] = draw_pos - right + up;
	v_glow_corner[1] = draw_pos - right - up;
	v_glow_corner[2] = draw_pos + right + up;
	v_glow_corner[3] = draw_pos + right - up;

	face = mFace[FACE_BLOOM]; 

	if (face->mVertexBuffer.isNull())
	{
		face->setSize(4, 6);
		face->setGeomIndex(0);
		face->setIndicesIndex(0);
		face->mVertexBuffer = new LLVertexBuffer(LLDrawPoolWater::VERTEX_DATA_MASK, GL_STREAM_DRAW_ARB);
		face->mVertexBuffer->allocateBuffer(4, 6, TRUE);
	}

	index_offset = face->getGeometry(verticesp,normalsp,texCoordsp, indicesp);
	if (-1 == index_offset)
	{
		return;
	}

	for (S32 vtx = 0; vtx < 4; ++vtx)
	{
		*(verticesp++)  = v_glow_corner[vtx] + mCameraPosAgent;
	}

	*(texCoordsp++) = TEX01;
	*(texCoordsp++) = TEX00;
	*(texCoordsp++) = TEX11;
	*(texCoordsp++) = TEX10;

	*indicesp++ = index_offset + 0;
	*indicesp++ = index_offset + 2;
	*indicesp++ = index_offset + 1;

	*indicesp++ = index_offset + 1;
	*indicesp++ = index_offset + 2;
	*indicesp++ = index_offset + 3;
#endif
}


F32 dtReflection(const LLVector3& p, F32 cos_dir_from_top, F32 sin_dir_from_top, F32 diff_angl_dir)
{
	LLVector3 P = p;
	P.normalize();

	const F32 cos_dir_angle = -P.mV[VZ];
	const F32 sin_dir_angle = sqrt(1 - cos_dir_angle * cos_dir_angle);

	F32 cos_diff_angles = cos_dir_angle * cos_dir_from_top
									+ sin_dir_angle * sin_dir_from_top;

	F32 diff_angles;
	if (cos_diff_angles > (1 - 1e-7))
		diff_angles = 0;
	else
		diff_angles = acos(cos_diff_angles);

	const F32 rel_diff_angles = diff_angles / diff_angl_dir;
	const F32 dt = 1 - rel_diff_angles;

	return (dt < 0) ? 0 : dt;
}


F32 dtClip(const LLVector3& v0, const LLVector3& v1, F32 far_clip2)
{
	F32 dt_clip;
	const LLVector3 otrezok = v1 - v0;
	const F32 A = otrezok.lengthSquared();
	const F32 B = v0 * otrezok;
	const F32 C = v0.lengthSquared() - far_clip2;
	const F32 det = sqrt(B*B - A*C);
	dt_clip = (-B - det) / A;
	if ((dt_clip < 0) || (dt_clip > 1))
		dt_clip = (-B + det) / A;
	return dt_clip;
}


void LLVOSky::updateReflectionGeometry(LLDrawable *drawable, F32 H,
										 const LLHeavenBody& HB)
{
	const LLVector3 &look_at = LLViewerCamera::getInstance()->getAtAxis();
	// const F32 water_height = gAgent.getRegion()->getWaterHeight() + 0.001f;
	// LLWorld::getInstance()->getWaterHeight() + 0.001f;

	LLVector3 to_dir = HB.getDirection();
	LLVector3 hb_pos = to_dir * (HORIZON_DIST - 10);
	LLVector3 to_dir_proj = to_dir;
	to_dir_proj.mV[VZ] = 0;
	to_dir_proj.normalize();

	LLVector3 Right = to_dir % LLVector3::z_axis;
	LLVector3 Up = Right % to_dir;
	Right.normalize();
	Up.normalize();

	// finding angle between  look direction and sprite.
	LLVector3 look_at_right = look_at % LLVector3::z_axis;
	look_at_right.normalize();

	const static F32 cos_horizon_angle = cosHorizon(0.0f/sResolution);
	//const static F32 horizon_angle = acos(cos_horizon_angle);

	const F32 enlargm_factor = ( 1 - to_dir.mV[2] );
	F32 horiz_enlargement = 1 + enlargm_factor * 0.3f;
	F32 vert_enlargement = 1 + enlargm_factor * 0.2f;

	F32 vert_size = vert_enlargement * HEAVENLY_BODY_SCALE * HB.getDiskRadius();
	Right *= /*cos_lookAt_toDir */ horiz_enlargement * HEAVENLY_BODY_SCALE * HB.getDiskRadius();
	Up *= vert_size;

	LLVector3 v_corner[2];
	LLVector3 stretch_corner[2];

	LLVector3 top_hb = v_corner[0] = stretch_corner[0] = hb_pos - Right + Up;
	v_corner[1] = stretch_corner[1] = hb_pos - Right - Up;

	F32 dt_hor, dt;
	dt_hor = clip_side_to_horizon(v_corner[1], v_corner[0], cos_horizon_angle);

	LLVector2 TEX0t = TEX00;
	LLVector2 TEX1t = TEX10;
	LLVector3 lower_corner = v_corner[1];

	if ((dt_hor > 0) && (dt_hor < 1))
	{
		TEX0t = LLVector2(0, dt_hor);
		TEX1t = LLVector2(1, dt_hor);
		lower_corner = (1 - dt_hor) * v_corner[1] + dt_hor * v_corner[0];
	}
	else
		dt_hor = llmax(0.0f, llmin(1.0f, dt_hor));

	top_hb.normalize();
	const F32 cos_angle_of_view = fabs(top_hb.mV[VZ]);
	const F32 extension = llmin (5.0f, 1.0f / cos_angle_of_view);

	const S32 cols = 1;
	const S32 raws = lltrunc(16 * extension);
	S32 quads = cols * raws;

	stretch_corner[0] = lower_corner + extension * (stretch_corner[0] - lower_corner);
	stretch_corner[1] = lower_corner + extension * (stretch_corner[1] - lower_corner);

	dt = dt_hor;


	F32 cos_dir_from_top[2];

	LLVector3 dir = stretch_corner[0];
	dir.normalize();
	cos_dir_from_top[0] = dir.mV[VZ];

	dir = stretch_corner[1];
	dir.normalize();
	cos_dir_from_top[1] = dir.mV[VZ];

	const F32 sin_dir_from_top = sqrt(1 - cos_dir_from_top[0] * cos_dir_from_top[0]);
	const F32 sin_dir_from_top2 = sqrt(1 - cos_dir_from_top[1] * cos_dir_from_top[1]);
	const F32 cos_diff_dir = cos_dir_from_top[0] * cos_dir_from_top[1]
							+ sin_dir_from_top * sin_dir_from_top2;
	const F32 diff_angl_dir = acos(cos_diff_dir);

	v_corner[0] = stretch_corner[0];
	v_corner[1] = lower_corner;


	LLVector2 TEX0tt = TEX01;
	LLVector2 TEX1tt = TEX11;

	LLVector3 v_refl_corner[4];
	LLVector3 v_sprite_corner[4];

	S32 vtx;
	for (vtx = 0; vtx < 2; ++vtx)
	{
		LLVector3 light_proj = v_corner[vtx];
		light_proj.normalize();

		const F32 z = light_proj.mV[VZ];
		const F32 sin_angle = sqrt(1 - z * z);
		light_proj *= 1.f / sin_angle;
		light_proj.mV[VZ] = 0;
		const F32 to_refl_point = H * sin_angle / fabs(z);

		v_refl_corner[vtx] = to_refl_point * light_proj;
	}


	for (vtx = 2; vtx < 4; ++vtx)
	{
		const LLVector3 to_dir_vec = (to_dir_proj * v_refl_corner[vtx-2]) * to_dir_proj;
		v_refl_corner[vtx] = v_refl_corner[vtx-2] + 2 * (to_dir_vec - v_refl_corner[vtx-2]);
	}

	for (vtx = 0; vtx < 4; ++vtx)
		v_refl_corner[vtx].mV[VZ] -= H;

	S32 side = 0;
	LLVector3 refl_corn_norm[2];
	refl_corn_norm[0] = v_refl_corner[1];
	refl_corn_norm[0].normalize();
	refl_corn_norm[1] = v_refl_corner[3];
	refl_corn_norm[1].normalize();

	F32 cos_refl_look_at[2];
	cos_refl_look_at[0] = refl_corn_norm[0] * look_at;
	cos_refl_look_at[1] = refl_corn_norm[1] * look_at;

	if (cos_refl_look_at[1] > cos_refl_look_at[0])
	{
		side = 2;
	}

	//const F32 far_clip = (LLViewerCamera::getInstance()->getFar() - 0.01) / far_clip_factor;
	const F32 far_clip = 512;
	const F32 far_clip2 = far_clip*far_clip;

	F32 dt_clip;
	F32 vtx_near2, vtx_far2;

	if ((vtx_far2 = v_refl_corner[side].lengthSquared()) > far_clip2)
	{
		// whole thing is sprite: reflection is beyond far clip plane.
		dt_clip = 1.1f;
		quads = 1;
	}
	else if ((vtx_near2 = v_refl_corner[side+1].lengthSquared()) > far_clip2)
	{
		// part is reflection, the rest is sprite.
		dt_clip = dtClip(v_refl_corner[side + 1], v_refl_corner[side], far_clip2);
		const LLVector3 P = (1 - dt_clip) * v_refl_corner[side + 1] + dt_clip * v_refl_corner[side];

		F32 dt_tex = dtReflection(P, cos_dir_from_top[0], sin_dir_from_top, diff_angl_dir);

		dt = dt_tex;
		TEX0tt = LLVector2(0, dt);
		TEX1tt = LLVector2(1, dt);
		quads++;
	}
	else
	{
		// whole thing is correct reflection.
		dt_clip = -0.1f;
	}

	LLFace *face = mFace[FACE_REFLECTION]; 

    if (face)
    {
        if (!face->getVertexBuffer() || quads * 4 != face->getGeomCount())
        {
            face->setSize(quads * 4, quads * 6);
            LLVertexBuffer* buff = new LLVertexBuffer(LLDrawPoolWater::VERTEX_DATA_MASK, GL_STREAM_DRAW_ARB);
			if (!buff->allocateBuffer(face->getGeomCount(), face->getIndicesCount(), TRUE))
			{
				LL_WARNS() << "Failed to allocate Vertex Buffer for vosky to "
					<< face->getGeomCount() << " vertices and "
					<< face->getIndicesCount() << " indices" << LL_ENDL;
			}
            face->setIndicesIndex(0);
            face->setGeomIndex(0);
            face->setVertexBuffer(buff);
        }

        LLStrider<LLVector3> verticesp;
        LLStrider<LLVector3> normalsp;
        LLStrider<LLVector2> texCoordsp;
        LLStrider<U16> indicesp;
        S32 index_offset;

        index_offset = face->getGeometry(verticesp, normalsp, texCoordsp, indicesp);
        if (-1 == index_offset)
        {
            return;
        }

        LLColor3 hb_col3 = HB.getInterpColor();
        hb_col3.clamp();
        const LLColor4 hb_col = LLColor4(hb_col3);

        const F32 min_attenuation = 0.4f;
        const F32 max_attenuation = 0.7f;
        const F32 attenuation = min_attenuation
            + cos_angle_of_view * (max_attenuation - min_attenuation);

        LLColor4 hb_refl_col = (1 - attenuation) * hb_col + attenuation * getSkyFogColor();
        face->setFaceColor(hb_refl_col);

        LLVector3 v_far[2];
        v_far[0] = v_refl_corner[1];
        v_far[1] = v_refl_corner[3];

        if (dt_clip > 0)
        {
            if (dt_clip >= 1)
            {
                for (S32 vtx = 0; vtx < 4; ++vtx)
                {
                    F32 ratio = far_clip / v_refl_corner[vtx].length();
                    *(verticesp++) = v_refl_corner[vtx] = ratio * v_refl_corner[vtx] + mCameraPosAgent;
                }
                const LLVector3 draw_pos = 0.25 *
                    (v_refl_corner[0] + v_refl_corner[1] + v_refl_corner[2] + v_refl_corner[3]);
                face->mCenterAgent = draw_pos;
            }
            else
            {
                F32 ratio = far_clip / v_refl_corner[1].length();
                v_sprite_corner[1] = v_refl_corner[1] * ratio;

                ratio = far_clip / v_refl_corner[3].length();
                v_sprite_corner[3] = v_refl_corner[3] * ratio;

                v_refl_corner[1] = (1 - dt_clip) * v_refl_corner[1] + dt_clip * v_refl_corner[0];
                v_refl_corner[3] = (1 - dt_clip) * v_refl_corner[3] + dt_clip * v_refl_corner[2];
                v_sprite_corner[0] = v_refl_corner[1];
                v_sprite_corner[2] = v_refl_corner[3];

                for (S32 vtx = 0; vtx < 4; ++vtx)
                {
                    *(verticesp++) = v_sprite_corner[vtx] + mCameraPosAgent;
                }

                const LLVector3 draw_pos = 0.25 *
                    (v_refl_corner[0] + v_sprite_corner[1] + v_refl_corner[2] + v_sprite_corner[3]);
                face->mCenterAgent = draw_pos;
            }

            *(texCoordsp++) = TEX0tt;
            *(texCoordsp++) = TEX0t;
            *(texCoordsp++) = TEX1tt;
            *(texCoordsp++) = TEX1t;

            *indicesp++ = index_offset + 0;
            *indicesp++ = index_offset + 2;
            *indicesp++ = index_offset + 1;

            *indicesp++ = index_offset + 1;
            *indicesp++ = index_offset + 2;
            *indicesp++ = index_offset + 3;

            index_offset += 4;
        }

        if (dt_clip < 1)
        {
            if (dt_clip <= 0)
            {
                const LLVector3 draw_pos = 0.25 *
                    (v_refl_corner[0] + v_refl_corner[1] + v_refl_corner[2] + v_refl_corner[3]);
                face->mCenterAgent = draw_pos;
            }

            const F32 raws_inv = 1.f / raws;
            const F32 cols_inv = 1.f / cols;
            LLVector3 left = v_refl_corner[0] - v_refl_corner[1];
            LLVector3 right = v_refl_corner[2] - v_refl_corner[3];
            left *= raws_inv;
            right *= raws_inv;

            F32 dt_raw = dt;

            for (S32 raw = 0; raw < raws; ++raw)
            {
                F32 dt_v0 = raw * raws_inv;
                F32 dt_v1 = (raw + 1) * raws_inv;
                const LLVector3 BL = v_refl_corner[1] + (F32)raw * left;
                const LLVector3 BR = v_refl_corner[3] + (F32)raw * right;
                const LLVector3 EL = BL + left;
                const LLVector3 ER = BR + right;
                dt_v0 = dt_raw;
                dt_raw = dt_v1 = dtReflection(EL, cos_dir_from_top[0], sin_dir_from_top, diff_angl_dir);
                for (S32 col = 0; col < cols; ++col)
                {
                    F32 dt_h0 = col * cols_inv;
                    *(verticesp++) = (1 - dt_h0) * EL + dt_h0 * ER + mCameraPosAgent;
                    *(verticesp++) = (1 - dt_h0) * BL + dt_h0 * BR + mCameraPosAgent;
                    F32 dt_h1 = (col + 1) * cols_inv;
                    *(verticesp++) = (1 - dt_h1) * EL + dt_h1 * ER + mCameraPosAgent;
                    *(verticesp++) = (1 - dt_h1) * BL + dt_h1 * BR + mCameraPosAgent;

                    *(texCoordsp++) = LLVector2(dt_h0, dt_v1);
                    *(texCoordsp++) = LLVector2(dt_h0, dt_v0);
                    *(texCoordsp++) = LLVector2(dt_h1, dt_v1);
                    *(texCoordsp++) = LLVector2(dt_h1, dt_v0);

                    *indicesp++ = index_offset + 0;
                    *indicesp++ = index_offset + 2;
                    *indicesp++ = index_offset + 1;

                    *indicesp++ = index_offset + 1;
                    *indicesp++ = index_offset + 2;
                    *indicesp++ = index_offset + 3;

                    index_offset += 4;
                }
            }
        }

        face->getVertexBuffer()->flush();
    }
}

void LLVOSky::updateFog(const F32 distance)
{
    LLVector3 toSun = getToSunLast();
    m_legacyAtmospherics.updateFog(distance, toSun);
}

void LLVOSky::initSunDirection(const LLVector3 &sun_dir, const LLVector3 &sun_ang_velocity)
{
	LLVector3 sun_direction = (sun_dir.length() == 0) ? LLVector3::x_axis : sun_dir;
	sun_direction.normalize();
	mSun.setDirection(sun_direction);
	mSun.renewDirection();
	mSun.setAngularVelocity(sun_ang_velocity);
	mMoon.setDirection(-mSun.getDirection());
	mMoon.renewDirection();
	mLastLightingDirection = mSun.getDirection();

	updateDirections();

	if ( !mInitialized )
	{
		init();
		LLSkyTex::stepCurrent();
	}		
}

void LLVOSky::setSunDirection(const LLVector3 &sun_dir, const LLVector3 &moon_dir)
{
	LLVector3 sun_direction = (sun_dir.length() == 0) ? LLVector3::x_axis : sun_dir;
	sun_direction.normalize();

	// Push the sun "South" as it approaches directly overhead so that we can always see bump mapping
	// on the upward facing faces of cubes.
	LLVector3 newDir = sun_direction;

	// Same as dot product with the up direction + clamp.
	F32 sunDot = llmax(0.f, newDir.mV[2]);
	sunDot *= sunDot;	

	// Create normalized vector that has the sunDir pushed south about an hour and change.
	LLVector3 adjustedDir = (newDir + LLVector3(0.f, -0.70711f, 0.70711f)) * 0.5f;

	// Blend between normal sun dir and adjusted sun dir based on how close we are
	// to having the sun overhead.
	mBumpSunDir = adjustedDir * sunDot + newDir * (1.0f - sunDot);
	mBumpSunDir.normalize();

	F32 dp = mLastLightingDirection * sun_direction;
	mSun.setDirection(sun_direction);

	mMoon.setDirection(moon_dir);
	updateDirections();

	if (dp < 0.995f) { //the sun jumped a great deal, update immediately
		mForceUpdate = TRUE;
	}
}

LLVector3 LLVOSky::getLightDirection() const
{
    return LLEnvironment::instance().getCurrentSky()->getLightDirection(); 
}

LLColor4U LLVOSky::getFadeColor() const
{ 
    return LLEnvironment::instance().getCurrentSky()->getFadeColor(); 
}

LLColor3 LLVOSky::getSunDiffuseColor() const 
{
    return LLEnvironment::instance().getCurrentSky()->getSunDiffuse();
}

LLColor3 LLVOSky::getMoonDiffuseColor() const 
{ 
    return LLEnvironment::instance().getCurrentSky()->getMoonDiffuse();
}

LLColor4 LLVOSky::getSunAmbientColor() const 
{ 
    return LLEnvironment::instance().getCurrentSky()->getSunAmbient();
}

LLColor4 LLVOSky::getMoonAmbientColor() const 
{ 
    return LLEnvironment::instance().getCurrentSky()->getMoonAmbient();
}

LLColor4 LLVOSky::getTotalAmbientColor() const 
{ 
    return LLEnvironment::instance().getCurrentSky()->getTotalAmbient();
}
