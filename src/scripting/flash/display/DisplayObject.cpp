/**************************************************************************
    Lightspark, a free flash player implementation

    Copyright (C) 2012-2013  Alessandro Pignotti (a.pignotti@sssup.it)

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**************************************************************************/

#include "scripting/abc.h"
#include "compat.h"
#include "swf.h"
#include "scripting/flash/display/DisplayObject.h"
#include "backends/rendering.h"
#include "backends/input.h"
#include "scripting/argconv.h"
#include "scripting/flash/geom/flashgeom.h"
#include "scripting/flash/geom/Matrix3D.h"
#include "scripting/flash/accessibility/flashaccessibility.h"
#include "scripting/flash/display/Bitmap.h"
#include "scripting/flash/display/BitmapData.h"
#include "scripting/flash/display/LoaderInfo.h"
#include "scripting/flash/geom/flashgeom.h"
#include "scripting/flash/filters/flashfilters.h"
#include "scripting/flash/filters/BevelFilter.h"
#include "scripting/flash/filters/BlurFilter.h"
#include "scripting/flash/filters/ColorMatrixFilter.h"
#include "scripting/flash/filters/ConvolutionFilter.h"
#include "scripting/flash/filters/DisplacementMapFilter.h"
#include "scripting/flash/filters/DropShadowFilter.h"
#include "scripting/flash/filters/GlowFilter.h"
#include "scripting/flash/filters/GradientBevelFilter.h"
#include "scripting/flash/filters/GradientGlowFilter.h"
#include "scripting/toplevel/Number.h"
#include <algorithm>

// adobe seems to use twips as the base of the internal coordinate system, so we have to "round" coordinates to twips
// TODO I think we should also use a twips-based coordinate system
#define ROUND_TO_TWIPS(v) v = number_t(int(v*20))/20.0

using namespace lightspark;
using namespace std;

ATOMIC_INT32(DisplayObject::instanceCount);

Vector2f DisplayObject::getXY()
{
	Vector2f ret;
	ret.x = getMatrix().getTranslateX();
	ret.y = getMatrix().getTranslateY();
	return ret;
}

bool DisplayObject::getBounds(number_t& xmin, number_t& xmax, number_t& ymin, number_t& ymax, const MATRIX& m, bool visibleOnly)
{
	if(!legacy && !isConstructed())
		return false;

	bool ret=boundsRect(xmin,xmax,ymin,ymax,visibleOnly);
	if(ret)
	{
		number_t tmpX[4];
		number_t tmpY[4];
		m.multiply2D(xmin,ymin,tmpX[0],tmpY[0]);
		m.multiply2D(xmax,ymin,tmpX[1],tmpY[1]);
		m.multiply2D(xmax,ymax,tmpX[2],tmpY[2]);
		m.multiply2D(xmin,ymax,tmpX[3],tmpY[3]);
		auto retX=minmax_element(tmpX,tmpX+4);
		auto retY=minmax_element(tmpY,tmpY+4);
		xmin=*retX.first;
		xmax=*retX.second;
		ymin=*retY.first;
		ymax=*retY.second;
	}
	return ret;
}

RectF DisplayObject::boundsRectWithRenderTransform(const MATRIX& matrix, bool includeOwnFilters, const MATRIX& initialMatrix)
{
	RectF bounds;
	boundsRectWithoutChildren(bounds.min.x, bounds.max.x, bounds.min.y, bounds.max.y, false);
	bounds *= matrix;
	if (is<DisplayObjectContainer>())
	{
		std::vector<_R<DisplayObject>> list;
		as<DisplayObjectContainer>()->cloneDisplayList(list);
		for (auto child : list)
		{
			MATRIX m = matrix.multiplyMatrix(child->getMatrix());
			bounds = bounds._union(child->boundsRectWithRenderTransform(m, true, initialMatrix));
		}
	}
	if (includeOwnFilters && !filters.isNull())
	{
		number_t filterborder = 0;
		for (uint32_t i = 0; i < filters->size(); i++)
		{
			asAtom f = asAtomHandler::invalidAtom;
			filters->at_nocheck(f,i);
			if (asAtomHandler::is<BitmapFilter>(f))
				filterborder = max(filterborder,asAtomHandler::as<BitmapFilter>(f)->getMaxFilterBorder());
		}
		bounds.min.x -= filterborder*initialMatrix.getScaleX();
		bounds.max.x += filterborder*initialMatrix.getScaleX();
		bounds.min.y -= filterborder*initialMatrix.getScaleY();
		bounds.max.y += filterborder*initialMatrix.getScaleY();
	}
	return bounds;
}

number_t DisplayObject::getNominalWidth()
{
	number_t xmin, xmax, ymin, ymax;

	if(!isConstructed())
		return 0;

	bool ret=boundsRect(xmin,xmax,ymin,ymax,false);
	return ret?(xmax-xmin):0;
}

number_t DisplayObject::getNominalHeight()
{
	number_t xmin, xmax, ymin, ymax;

	if(!isConstructed())
		return 0;

	bool ret=boundsRect(xmin,xmax,ymin,ymax,false);
	return ret?(ymax-ymin):0;
}

bool DisplayObject::inMask() const
{
	if (mask.getPtr() || this->getClipDepth())
		return true;
	if (parent)
		return parent->inMask();
	return false;
}
bool DisplayObject::belongsToMask() const
{
	if (ismask)
		return true;
	if (parent)
		return parent->belongsToMask();
	return false;
}
bool DisplayObject::Render(RenderContext& ctxt, bool force,const MATRIX* startmatrix, RenderDisplayObjectToBitmapContainer* container)
{
	if((!legacy && !isConstructed()) || (!force && skipRender()) || clippedAlpha()==0.0 || (isMask() && !ClipDepth))
		return false;
	bool ret = true;
	const CachedSurface& surface = ctxt.getCachedSurface(this);
	if (!surface.getState())
		return false;
	if (!mask.isNull() && !mask->getClipDepth())
	{
		const CachedSurface& masksurface = ctxt.getCachedSurface(mask.getPtr());
		if (!masksurface.getState())
			return false;
	}
	MATRIX _matrix;
	if (startmatrix)
		_matrix = *startmatrix;
	else
		_matrix = surface.getState()->matrix;
	if (this->scrollRect)
		_matrix.translate(-this->scrollRect->x,-this->scrollRect->y);
	
	if (container)
	{
		ctxt.transformStack().push(Transform2D(
			_matrix,
			container->ct ? *container->ct : surface.getState()->colortransform,
			container->blendMode
			));
		
	}
	else
	{
		ctxt.transformStack().push(Transform2D(
			_matrix,
			surface.getState()->colortransform,
			surface.getState()->blendmode
			));
		
	}
	EngineData* engineData = getSystemState()->getEngineData();
	bool needscachedtexture = (!container && surface.getState()->cacheAsBitmap)
							  || ctxt.transformStack().transform().blendmode == BLENDMODE_LAYER
							  || ctxt.isMaskActive()
							  || hasFilters()
							  || (surface.getState()->needsLayer && getSystemState()->getRenderThread()->filterframebufferstack.empty());
	if (needscachedtexture && (surface.getState()->needsFilterRefresh || surface.cachedFilterTextureID != UINT32_MAX))
	{
		if (!surface.isInitialized)
		{
			LOG(LOG_ERROR,"uninitialzed surface:"<<this->toDebugString());
			ctxt.transformStack().pop();
			return true;
		}
		bool needsFilterRefresh = surface.getState()->needsFilterRefresh && needscachedtexture;
		auto baseTransform = ctxt.transformStack().transform();
		Vector2f scale = getSystemState()->getRenderThread()->getScale();
		MATRIX initialMatrix;
		initialMatrix.scale(scale.x, scale.y);
		RectF bounds = boundsRectWithRenderTransform(baseTransform.matrix, true, initialMatrix);
		Vector2f offset(bounds.min.x-baseTransform.matrix.x0,bounds.min.y-baseTransform.matrix.y0);
		Vector2f size = bounds.size();
		
		if (needsFilterRefresh)
		{
			MATRIX m = baseTransform.matrix;
			m.x0 = -offset.x;
			m.y0 = -offset.y;
			
			// TODO: Create a new GLRenderContext here
			ctxt.createTransformStack();
			ctxt.transformStack().push(Transform2D(m,ColorTransformBase(),AS_BLENDMODE::BLENDMODE_NORMAL));
			this->renderFilters(ctxt,size.x,size.y);
			
			ctxt.transformStack().pop();
			ctxt.removeTransformStack();
			surface.getState()->needsFilterRefresh=false;
		}
		if (surface.cachedFilterTextureID != UINT32_MAX)
		{
			MATRIX m;
			m.x0 = std::round(baseTransform.matrix.x0+offset.x);
			m.y0 = std::round(baseTransform.matrix.y0+offset.y);
			
			if (isShaderBlendMode(surface.getState()->blendmode))
			{
				assert (!getSystemState()->getRenderThread()->filterframebufferstack.empty());
				filterstackentry feparent = getSystemState()->getRenderThread()->filterframebufferstack.back();
				// set original texture as blend texture
				engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_BLEND);
				engineData->exec_glBindTexture_GL_TEXTURE_2D(feparent.filtertextureID);
			}
			getSystemState()->getRenderThread()->setModelView(m);
			getSystemState()->getRenderThread()->setupRenderingState(surface.getState()->alpha,ctxt.transformStack().transform().colorTransform,surface.getState()->smoothing,surface.getState()->blendmode);
			getSystemState()->getRenderThread()->renderTextureToFrameBuffer(surface.cachedFilterTextureID,size.x,size.y,nullptr,nullptr,false,true,false);
			ctxt.transformStack().pop();
			return ret;
		}
	}

	MATRIX maskMatrix;
	if (!mask.isNull() && !mask->getClipDepth())
	{
		MATRIX globalMatrix = getConcatenatedMatrix();
		maskMatrix = globalMatrix.isInvertible() ? globalMatrix.getInverted() : MATRIX();
		maskMatrix = maskMatrix.multiplyMatrix(mask->getConcatenatedMatrix());
		ctxt.transformStack().push(Transform2D(maskMatrix, ColorTransformBase(),AS_BLENDMODE::BLENDMODE_NORMAL));
		ctxt.pushMask();
		mask->renderImpl(ctxt);
		ctxt.transformStack().pop();
		ctxt.activateMask();
	}
	ret = renderImpl(ctxt);
	if (!mask.isNull() && !mask->getClipDepth())
	{
		ctxt.deactivateMask();
		ctxt.popMask();
	}
	ctxt.transformStack().pop();
	return ret;
}

DisplayObject::DisplayObject(ASWorker* wrk, Class_base* c):EventDispatcher(wrk,c),matrix(Class<Matrix>::getInstanceS(wrk)),tx(0),ty(0),rotation(0),
	sx(1),sy(1),alpha(1.0),blendMode(BLENDMODE_NORMAL),isLoadedRoot(false),ismask(false),maxfilterborder(0),ClipDepth(0),
	avm1PrevDisplayObject(nullptr),avm1NextDisplayObject(nullptr),parent(nullptr),constructed(false),useLegacyMatrix(true),
	needsTextureRecalculation(true),textureRecalculationSkippable(false),
	avm1mouselistenercount(0),avm1framelistenercount(0),
	onStage(false),visible(true),
	mask(),invalidateQueueNext(),loaderInfo(),loadedFrom(wrk->rootClip.getPtr()),hasChanged(true),legacy(false),placeFrame(UINT32_MAX),markedForLegacyDeletion(false),cacheAsBitmap(false),placedByActionScript(false),skipFrame(false),
	name(BUILTIN_STRINGS::EMPTY)
{
	subtype=SUBTYPE_DISPLAYOBJECT;
}

void DisplayObject::markAsChanged()
{
	hasChanged = true;
	if (onStage)
		requestInvalidation(getSystemState());
	else
		requestInvalidationFilterParent();
}

DisplayObject::~DisplayObject()
{
}

void DisplayObject::finalize()
{
	getSystemState()->stage->AVM1RemoveDisplayObject(this);
	removeAVM1Listeners();
	EventDispatcher::finalize();
	cachedBitmap.reset();
	parent=nullptr;
	eventparentmap.clear();
	mask.reset();
	matrix.reset();
	loaderInfo.reset();
	colorTransform.reset();
	invalidateQueueNext.reset();
	accessibilityProperties.reset();
	scalingGrid.reset();
	for (auto it = avm1variables.begin(); it != avm1variables.end(); it++)
	{
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			o->removeStoredMember();
	}
	avm1variables.clear();
	variablebindings.clear();
	loadedFrom=getSystemState()->mainClip;
	hasChanged = true;
	needsTextureRecalculation=true;
	if (!cachedSurface.isChunkOwner)
		cachedSurface.tex=nullptr;
	cachedSurface.isChunkOwner=true;
	cachedSurface.isValid=false;
	cachedSurface.isInitialized=false;
	cachedSurface.wasUpdated=false;
	avm1mouselistenercount=0;
	avm1framelistenercount=0;
	EventDispatcher::finalize();
}

bool DisplayObject::destruct()
{
	// TODO make all DisplayObject derived classes reusable
	getSystemState()->stage->AVM1RemoveDisplayObject(this);
	removeAVM1Listeners();
	cachedBitmap.reset();
	ismask=false;
	maxfilterborder=0;
	parent=nullptr;
	eventparentmap.clear();
	mask.reset();
	matrix.reset();
	loaderInfo.reset();
	invalidateQueueNext.reset();
	accessibilityProperties.reset();
	colorTransform.reset();
	scalingGrid.reset();
	scrollRect.reset();
	loadedFrom=getSystemState()->mainClip;
	hasChanged = true;
	needsTextureRecalculation=true;
	tx=0;
	ty=0;
	rotation=0;
	sx=1;
	sy=1;
	alpha=1.0;
	blendMode=BLENDMODE_NORMAL;
	isLoadedRoot=false;
	ClipDepth=0;
	constructed=false;
	useLegacyMatrix=true;
	onStage=false;
	visible=true;
	avm1mouselistenercount=0;
	avm1framelistenercount=0;
	filters.reset();
	legacy=false;
	markedForLegacyDeletion=false;
	cacheAsBitmap=false;
	name=BUILTIN_STRINGS::EMPTY;
	for (auto it = avm1variables.begin(); it != avm1variables.end(); it++)
	{
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			o->removeStoredMember();
	}
	avm1variables.clear();
	variablebindings.clear();
	if (!cachedSurface.isChunkOwner)
		cachedSurface.tex=nullptr;
	if (cachedSurface.tex)
		cachedSurface.tex->makeEmpty();
	if (cachedSurface.getState())
		cachedSurface.getState()->reset();
	cachedSurface.isChunkOwner=true;
	cachedSurface.isValid=false;
	cachedSurface.isInitialized=false;
	cachedSurface.wasUpdated=false;
	if (cachedSurface.cachedFilterTextureID != UINT32_MAX && getSystemState() && getSystemState()->getRenderThread())
		getSystemState()->getRenderThread()->addDeletedTexture(cachedSurface.cachedFilterTextureID);
	cachedSurface.cachedFilterTextureID=UINT32_MAX;
	placeFrame=UINT32_MAX;
	return EventDispatcher::destruct();
}

void DisplayObject::prepareShutdown()
{
	if (preparedforshutdown)
		return;
	EventDispatcher::prepareShutdown();

	if (cachedSurface.mask)
		cachedSurface.mask->prepareShutdown();
	if (cachedBitmap)
		cachedBitmap->prepareShutdown();
	if (mask)
		mask->prepareShutdown();
	if (matrix)
		matrix->prepareShutdown();;
	if (loaderInfo)
		loaderInfo->prepareShutdown();;
	if (invalidateQueueNext)
		invalidateQueueNext->prepareShutdown();
	if (accessibilityProperties)
		accessibilityProperties->prepareShutdown();
	if (colorTransform)
		colorTransform->prepareShutdown();
	if (scalingGrid)
		scalingGrid->prepareShutdown();
	if (filters)
		filters->prepareShutdown();
	if (scrollRect)
		scrollRect->prepareShutdown();
	for (auto it = avm1variables.begin(); it != avm1variables.end(); it++)
	{
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			o->prepareShutdown();
	}
}

bool DisplayObject::countCylicMemberReferences(garbagecollectorstate& gcstate)
{
	if (gcstate.checkAncestors(this))
		return false;
	bool ret = EventDispatcher::countCylicMemberReferences(gcstate);
	for (auto it = avm1variables.begin(); it != avm1variables.end(); it++)
	{
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			ret = o->countAllCylicMemberReferences(gcstate) || ret;
	}
	return ret;
}

void DisplayObject::sinit(Class_base* c)
{
	CLASS_SETUP(c, EventDispatcher, _constructorNotInstantiatable, CLASS_SEALED);
	c->setDeclaredMethodByQName("loaderInfo","",Class<IFunction>::getFunction(c->getSystemState(),_getLoaderInfo,0,Class<LoaderInfo>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("width","",Class<IFunction>::getFunction(c->getSystemState(),_getWidth,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("width","",Class<IFunction>::getFunction(c->getSystemState(),_setWidth),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleX","",Class<IFunction>::getFunction(c->getSystemState(),_getScaleX,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleX","",Class<IFunction>::getFunction(c->getSystemState(),_setScaleX),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleY","",Class<IFunction>::getFunction(c->getSystemState(),_getScaleY,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleY","",Class<IFunction>::getFunction(c->getSystemState(),_setScaleY),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleZ","",Class<IFunction>::getFunction(c->getSystemState(),_getScaleZ,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("scaleZ","",Class<IFunction>::getFunction(c->getSystemState(),_setScaleZ),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("x","",Class<IFunction>::getFunction(c->getSystemState(),_getX,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("x","",Class<IFunction>::getFunction(c->getSystemState(),_setX),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("y","",Class<IFunction>::getFunction(c->getSystemState(),_getY,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("y","",Class<IFunction>::getFunction(c->getSystemState(),_setY),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("z","",Class<IFunction>::getFunction(c->getSystemState(),_getZ,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("z","",Class<IFunction>::getFunction(c->getSystemState(),_setZ),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("height","",Class<IFunction>::getFunction(c->getSystemState(),_getHeight,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("height","",Class<IFunction>::getFunction(c->getSystemState(),_setHeight),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("visible","",Class<IFunction>::getFunction(c->getSystemState(),_getVisible,0,Class<Boolean>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("visible","",Class<IFunction>::getFunction(c->getSystemState(),_setVisible),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("rotation","",Class<IFunction>::getFunction(c->getSystemState(),_getRotation,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("rotation","",Class<IFunction>::getFunction(c->getSystemState(),_setRotation),SETTER_METHOD,true);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,name,ASString);
	c->setDeclaredMethodByQName("parent","",Class<IFunction>::getFunction(c->getSystemState(),_getParent,0,Class<DisplayObject>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("root","",Class<IFunction>::getFunction(c->getSystemState(),_getRoot,0,Class<DisplayObject>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("blendMode","",Class<IFunction>::getFunction(c->getSystemState(),_getBlendMode,0,Class<ASString>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("blendMode","",Class<IFunction>::getFunction(c->getSystemState(),_setBlendMode),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("scale9Grid","",Class<IFunction>::getFunction(c->getSystemState(),_getScale9Grid,0,Class<Rectangle>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("scale9Grid","",Class<IFunction>::getFunction(c->getSystemState(),_setScale9Grid),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("stage","",Class<IFunction>::getFunction(c->getSystemState(),_getStage,0,Class<DisplayObject>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("mask","",Class<IFunction>::getFunction(c->getSystemState(),_getMask,0,Class<DisplayObject>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("mask","",Class<IFunction>::getFunction(c->getSystemState(),_setMask),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("alpha","",Class<IFunction>::getFunction(c->getSystemState(),_getAlpha,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("alpha","",Class<IFunction>::getFunction(c->getSystemState(),_setAlpha),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("getBounds","",Class<IFunction>::getFunction(c->getSystemState(),_getBounds,1,Class<Rectangle>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("getRect","",Class<IFunction>::getFunction(c->getSystemState(),_getBounds,1,Class<Rectangle>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("mouseX","",Class<IFunction>::getFunction(c->getSystemState(),_getMouseX,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("mouseY","",Class<IFunction>::getFunction(c->getSystemState(),_getMouseY,0,Class<Number>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("localToGlobal","",Class<IFunction>::getFunction(c->getSystemState(),localToGlobal,1,Class<Point>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("globalToLocal","",Class<IFunction>::getFunction(c->getSystemState(),globalToLocal,1,Class<Point>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("hitTestObject","",Class<IFunction>::getFunction(c->getSystemState(),hitTestObject,1,Class<Boolean>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("hitTestPoint","",Class<IFunction>::getFunction(c->getSystemState(),hitTestPoint,2,Class<Boolean>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("transform","",Class<IFunction>::getFunction(c->getSystemState(),_getTransform,0,Class<Transform>::getRef(c->getSystemState()).getPtr()),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("transform","",Class<IFunction>::getFunction(c->getSystemState(),_setTransform),SETTER_METHOD,true);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,accessibilityProperties,AccessibilityProperties);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,cacheAsBitmap,Boolean);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,filters,Array);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,scrollRect,Rectangle);
	REGISTER_GETTER_SETTER_RESULTTYPE(c, rotationX,Number);
	REGISTER_GETTER_SETTER_RESULTTYPE(c, rotationY,Number);
	REGISTER_GETTER_SETTER_RESULTTYPE(c, opaqueBackground,ASObject);
	REGISTER_GETTER_SETTER_RESULTTYPE(c, metaData,ASObject);

	c->addImplementedInterface(InterfaceClass<IBitmapDrawable>::getClass(c->getSystemState()));
	IBitmapDrawable::linkTraits(c);
}

ASFUNCTIONBODY_GETTER_SETTER_STRINGID_CB(DisplayObject,name,onSetName)
ASFUNCTIONBODY_GETTER_SETTER(DisplayObject,accessibilityProperties)
ASFUNCTIONBODY_GETTER_SETTER_CB(DisplayObject,scrollRect,onSetScrollRect)
ASFUNCTIONBODY_GETTER_SETTER_NOT_IMPLEMENTED(DisplayObject, rotationX)
ASFUNCTIONBODY_GETTER_SETTER_NOT_IMPLEMENTED(DisplayObject, rotationY)
ASFUNCTIONBODY_GETTER_SETTER_NOT_IMPLEMENTED(DisplayObject, opaqueBackground)
ASFUNCTIONBODY_GETTER_SETTER_NOT_IMPLEMENTED(DisplayObject, metaData)

void DisplayObject::onSetName(uint32_t oldName)
{
	if (!needsActionScript3() && oldName != name)
	{
		auto parent = getParent();
		if (parent != nullptr)
		{
			bool set = false;
			multiname m(nullptr);
			m.name_type = multiname::NAME_STRING;
			m.isAttribute = false;
			m.name_s_id = name;

			ASWorker* wrk = parent->getInstanceWorker();
			variable* v = parent->findVariableByMultiname(m,parent->getClass(),nullptr,nullptr,true,wrk);
			if (v != nullptr && asAtomHandler::is<DisplayObject>(v->var))
			{
				auto obj = asAtomHandler::as<DisplayObject>(v->var);
				if (parent->findLegacyChildDepth(this) < parent->findLegacyChildDepth(obj))
					set = true;
			}

			if (set || v == nullptr)
			{
				incRef();
				asAtom val = asAtomHandler::fromObject(this);
				parent->setVariableByMultiname(m, val, ASObject::CONST_NOT_ALLOWED, nullptr, wrk);
			}

			if (oldName != BUILTIN_STRINGS::EMPTY && v != nullptr)
			{
				m.name_s_id = oldName;
				v->setVar(wrk, asAtomHandler::undefinedAtom, false);
				parent->deleteVariableByMultiname(m, wrk);
			}
		}
	}
}
void DisplayObject::onSetScrollRect(_NR<Rectangle> oldValue)
{
	if (oldValue == this->scrollRect)
		return;
	if (!oldValue.isNull())
		oldValue->removeUser(this);
	if (!this->scrollRect.isNull())
	{
		// not mentioned in the specs, but setting scrollRect actually creates a clone of the provided rect,
		// so that any future changes to the scrollRect have no effect on the provided rect and vice versa
		Rectangle* res=Class<Rectangle>::getInstanceS(this->getInstanceWorker());
		res->x=this->scrollRect->x;
		res->y=this->scrollRect->y;
		res->width=this->scrollRect->width;
		res->height=this->scrollRect->height;
		
		this->scrollRect = _MR(res);
		this->scrollRect->addUser(this);
	}
	if ((this->scrollRect.isNull() && !oldValue.isNull()) ||
		(!this->scrollRect.isNull() && oldValue.isNull()) ||
		(!this->scrollRect.isNull() && !oldValue.isNull() &&
		 (this->scrollRect->x != oldValue->x ||
		  this->scrollRect->y != oldValue->y ||
		  this->scrollRect->width != oldValue->width ||
		  this->scrollRect->height != oldValue->height)))
	{
		hasChanged = true;
		if (onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}
void DisplayObject::updatedRect()
{
	assert(!this->scrollRect.isNull());
	hasChanged = true;
	if (onStage)
		requestInvalidation(getSystemState());
	else
		requestInvalidationFilterParent();
	
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getter_filters)
{
	if(!asAtomHandler::is<DisplayObject>(obj))
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Function applied to wrong object");
		return;
	}
	if(argslen != 0)
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Arguments provided in getter");
		return;
	}
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if (th->filters.isNull())
		th->filters = _MR(Class<Array>::getInstanceSNoArgs(wrk));
	th->filters->incRef();
	ret = asAtomHandler::fromObject(th->filters.getPtr());
}
ASFUNCTIONBODY_ATOM(DisplayObject,_setter_filters)
{
	if(!asAtomHandler::is<DisplayObject>(obj))
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Function applied to wrong object");
		return;
	}
	if(argslen != 1)
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Arguments provided in getter");
		return;
	}
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);

	th->filters =ArgumentConversionAtom<_NR<Array>>::toConcrete(wrk,args[0],th->filters);
	th->maxfilterborder=0;
	for (uint32_t i = 0; i < th->filters->size(); i++)
	{
		asAtom f = asAtomHandler::invalidAtom;
		th->filters->at_nocheck(f,i);
		if (asAtomHandler::is<BitmapFilter>(f))
			th->maxfilterborder = max(th->maxfilterborder,asAtomHandler::as<BitmapFilter>(f)->getMaxFilterBorder());
	}
	th->requestInvalidation(wrk->getSystemState());
}
bool DisplayObject::computeCacheAsBitmap(bool checksize)
{
	// TODO handle cacheAsBitmap case through filter renderer
	return false;
	if (cacheAsBitmap || blendMode == BLENDMODE_LAYER)
	{
		if (checksize)
		{
			number_t bxmin,bxmax,bymin,bymax;
			boundsRect(bxmin,bxmax,bymin,bymax,true);
			// check if size of resulting bitmap is too large (see Adobe reference for DisplayObject.cacheAsBitmap)
			uint32_t w=(ceil(bxmax-bxmin));
			uint32_t h=(ceil(bymax-bymin));
			return (w <= 8192 && h <= 8192 && (w * h) <= 16777216);
		}
		return true;
	}
	return false;
}

bool DisplayObject::needsCacheAsBitmap() const
{
	return cacheAsBitmap
		   || blendMode == BLENDMODE_MULTIPLY
		   || blendMode == BLENDMODE_ADD
		   || blendMode == BLENDMODE_SCREEN
		   || blendMode == BLENDMODE_DARKEN
		   || blendMode == BLENDMODE_DIFFERENCE
		   || blendMode == BLENDMODE_HARDLIGHT
		   || blendMode == BLENDMODE_LIGHTEN
		   || blendMode == BLENDMODE_OVERLAY
		   || blendMode == BLENDMODE_ERASE
		   || hasFilters();
}
bool DisplayObject::hasFilters() const
{
	return filters && filters->size();
}

void DisplayObject::requestInvalidationFilterParent(InvalidateQueue* q)
{
	if (cachedSurface.cachedFilterTextureID != UINT32_MAX
		|| (!cachedSurface.isInitialized && (this->hasFilters()
											 || this->inMask()
											 || isShaderBlendMode(getBlendMode()))
		))
	{
		if (cachedSurface.getState())
			cachedSurface.getState()->needsFilterRefresh=true;
		this->hasChanged=true;
		requestInvalidationIncludingChildren(q);
	}
	DisplayObject* p = getParent();
	while (p)
	{
		if (p->cachedSurface.cachedFilterTextureID != UINT32_MAX
			|| (!p->cachedSurface.isInitialized && (p->hasFilters()
													|| p->inMask()
													|| isShaderBlendMode(p->getBlendMode()))
			))
		{
			p->requestInvalidationFilterParent(q);
			break;
		}	
		p = p->getParent();
	}
}
void DisplayObject::requestInvalidationIncludingChildren(InvalidateQueue* q)
{
	this->hasChanged=true;
	if (q)
	{
		this->incRef();
		q->addToInvalidateQueue(_MR(this));
	}
}
ASFUNCTIONBODY_ATOM(DisplayObject,_getter_cacheAsBitmap)
{
	if(!asAtomHandler::is<DisplayObject>(obj))
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Function applied to wrong object");
		return;
	}
	if(argslen != 0)
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Arguments provided in getter");
		return;
	}
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	ret = asAtomHandler::fromBool(th->cacheAsBitmap);
}
ASFUNCTIONBODY_ATOM(DisplayObject,_setter_cacheAsBitmap)
{
	if(!asAtomHandler::is<DisplayObject>(obj))
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Function applied to wrong object");
		return;
	}
	if(argslen != 1)
	{
		createError<ArgumentError>(wrk,kInvalidArgumentError,"Arguments provided in getter");
		return;
	}
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if (th->cacheAsBitmap != asAtomHandler::toInt(args[0]))
	{
		th->hasChanged=true;
		th->cacheAsBitmap = asAtomHandler::toInt(args[0]);
		th->requestInvalidation(wrk->getSystemState());
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getTransform)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	
	th->incRef();
	ret = asAtomHandler::fromObject(Class<Transform>::getInstanceS(wrk,_MR(th)));
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setTransform)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	_NR<Transform> trans;
	ARG_CHECK(ARG_UNPACK(trans));
	if (!trans.isNull())
	{
		th->setMatrix(trans->owner->matrix);
		th->colorTransform = trans->owner->colorTransform;
		th->hasChanged=true;
	}
}

void DisplayObject::setMatrix(_NR<Matrix> m)
{
	bool mustInvalidate=false;
	if (m.isNull())
	{
		if (!matrix.isNull())
		{
			mustInvalidate=true;
			geometryChanged();
		}
		matrix= NullRef;
	}
	else
	{
		Locker locker(spinlock);
		if (matrix.isNull())
			matrix= _MR(Class<Matrix>::getInstanceS(this->getInstanceWorker()));
		if(matrix->matrix!=m->matrix)
		{
			matrix->matrix=m->matrix;
			extractValuesFromMatrix();
			geometryChanged();
			mustInvalidate=true;
		}
	}
	if(mustInvalidate)
	{
		hasChanged=true;
		if (onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

void DisplayObject::setMatrix3D(_NR<Matrix3D> m)
{
	bool mustInvalidate=false;
	if (!m.isNull())
	{
		Locker locker(spinlock);
		matrix= NullRef;
		float rawdata[16];
		m->getRawDataAsFloat(rawdata);
		this->tx = rawdata[12];
		this->ty = rawdata[13];
		this->tz = rawdata[14];
		this->sx = rawdata[0];
		this->sy = rawdata[5];
		this->sz = rawdata[10];
		LOG(LOG_NOT_IMPLEMENTED,"not all values of Matrix3D are handled in DisplayObject");
		geometryChanged();
		mustInvalidate=true;
	}
	if(mustInvalidate)
	{
		hasChanged=true;
		if (onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

void DisplayObject::setLegacyMatrix(const lightspark::MATRIX& m)
{
	if(!useLegacyMatrix)
		return;
	bool mustInvalidate=false;
	{
		Locker locker(spinlock);
		if (matrix.isNull())
			matrix= _MR(Class<Matrix>::getInstanceS(this->getInstanceWorker()));
		if(m.getTranslateX() != tx ||
			m.getTranslateY() != ty ||
			m.getScaleX() != sx ||
			m.getScaleY() != sy ||
			m.getRotation() != rotation)
		{
			matrix->matrix=m;
			extractValuesFromMatrix();
			geometryChanged();
			afterSetLegacyMatrix();
			mustInvalidate=true;
		}
	}
	if(mustInvalidate)
	{
		hasChanged=true;
		if (onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

void DisplayObject::setFilters(const FILTERLIST& filterlist)
{
	if (filterlist.Filters.size())
	{
		if (filters.isNull())
			filters = _MR(Class<Array>::getInstanceS(getInstanceWorker()));
		else
		{
			// check if filterlist has really changed
			if (filters->size() == filterlist.Filters.size())
			{
				bool filterlistHasChanged=false;
				for (uint32_t i =0; i < filters->size(); i++)
				{
					asAtom f=filters->at(i);
					if (!asAtomHandler::is<BitmapFilter>(f))
					{
						filterlistHasChanged=true;
						break;
					}
					if (!asAtomHandler::as<BitmapFilter>(f)->compareFILTER(filterlist.Filters.at(i)))
					{
						filterlistHasChanged=true;
						break;
					}
				}
				if (!filterlistHasChanged)
					return;
			}
		}
		maxfilterborder=0;
		filters->resize(0);
		auto it = filterlist.Filters.cbegin();
		while (it != filterlist.Filters.cend())
		{
			BitmapFilter* f = nullptr;
			switch(it->FilterID)
			{
				case FILTER::FILTER_DROPSHADOW:
					f=Class<DropShadowFilter>::getInstanceS(getInstanceWorker(),it->DropShadowFilter);
					break;
				case FILTER::FILTER_BLUR:
					f=Class<BlurFilter>::getInstanceS(getInstanceWorker(),it->BlurFilter);
					break;
				case FILTER::FILTER_GLOW:
					f=Class<GlowFilter>::getInstanceS(getInstanceWorker(),it->GlowFilter);
					break;
				case FILTER::FILTER_BEVEL:
					f=Class<BevelFilter>::getInstanceS(getInstanceWorker(),it->BevelFilter);
					break;
				case FILTER::FILTER_GRADIENTGLOW:
					f=Class<GradientGlowFilter>::getInstanceS(getInstanceWorker(),it->GradientGlowFilter);
					break;
				case FILTER::FILTER_CONVOLUTION:
					f=Class<ConvolutionFilter>::getInstanceS(getInstanceWorker(),it->ConvolutionFilter);
					break;
				case FILTER::FILTER_COLORMATRIX:
					f=Class<ColorMatrixFilter>::getInstanceS(getInstanceWorker(),it->ColorMatrixFilter);
					break;
				case FILTER::FILTER_GRADIENTBEVEL:
					f=Class<GradientBevelFilter>::getInstanceS(getInstanceWorker(),it->GradientBevelFilter);
					break;
				default:
					LOG(LOG_ERROR,"Unsupported Filter Id " << (int)it->FilterID);
					break;
			}
			if (f)
			{
				filters->push(asAtomHandler::fromObject(f));
				maxfilterborder = max(maxfilterborder,f->getMaxFilterBorder());
			}
			it++;
		}
		hasChanged=true;
		setNeedsTextureRecalculation();
		requestInvalidation(getSystemState());
	}
	else
	{
		maxfilterborder=0;
		if (!filters.isNull() && filters->size())
		{
			filters->resize(0);
			hasChanged=true;
			setNeedsTextureRecalculation();
			requestInvalidation(getSystemState());
		}
	}
}

void DisplayObject::setMask(_NR<DisplayObject> m)
{
	bool mustInvalidate=(mask!=m || (m && m->hasChanged));

	if(!mask.isNull())
	{
		//Remove previous mask
		mask->ismask=false;
	}

	mask=m;
	if(!mask.isNull())
	{
		//Use new mask
		mask->ismask=true;
	}

	if(mustInvalidate)
	{
		hasChanged=true;
		if (onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}
void DisplayObject::setBlendMode(UI8 blendmode)
{
	if (blendmode <= 1 || blendmode > 14)
		this->blendMode = BLENDMODE_NORMAL;
	else
	{
		this->blendMode = (AS_BLENDMODE)(uint8_t)blendmode;
	}
}

bool DisplayObject::isShaderBlendMode(AS_BLENDMODE bl)
{
	// TODO add shader for other extended blendmodes
	return bl == AS_BLENDMODE::BLENDMODE_OVERLAY
			|| bl == BLENDMODE_HARDLIGHT;
}
MATRIX DisplayObject::getConcatenatedMatrix(bool includeRoot) const
{
	if(!parent || (!includeRoot && parent == getSystemState()->mainClip))
		return getMatrix();
	else
		return parent->getConcatenatedMatrix(includeRoot).multiplyMatrix(getMatrix());
}

/* Return alpha value between 0 and 1. (The stored alpha value is not
 * necessary bounded.) */
float DisplayObject::clippedAlpha() const
{
	float a = alpha;
	if (!this->colorTransform.isNull())
	{
		if (alpha != 1.0 && this->colorTransform->alphaMultiplier != 1.0)
			a = alpha * this->colorTransform->alphaMultiplier /256.;
		if (this->colorTransform->alphaOffset != 0)
			a = alpha + this->colorTransform->alphaOffset /256.;
	}
	return dmin(dmax(a, 0.), 1.);
}

float DisplayObject::getConcatenatedAlpha() const
{
	if(!parent)
		return clippedAlpha();
	else
		return parent->getConcatenatedAlpha()*clippedAlpha();
}

void DisplayObject::onNewEvent(Event* ev)
{
	Locker locker(spinlock);
	if (parent && parent->getInDestruction())
		return;
	if (parent)
	{
		eventparentmap[ev] =parent;
		parent->incRef();
	}
}

void DisplayObject::afterHandleEvent(Event *ev)
{
	Locker locker(spinlock);
	auto it = eventparentmap.find(ev);
	if (it != eventparentmap.end())
	{
		it->second->decRef();
		eventparentmap.erase(it);
	}
}

tiny_string DisplayObject::AVM1GetPath()
{
	tiny_string res;
	if (getParent())
		res = getParent()->AVM1GetPath();
	if (this->name != BUILTIN_STRINGS::EMPTY)
	{
		if (!res.empty())
			res += ".";
		res += getSystemState()->getStringFromUniqueId(this->name);
	}
	return res;
}

void DisplayObject::afterLegacyInsert()
{
}

MATRIX DisplayObject::getMatrix(bool includeRotation) const
{
	Locker locker(spinlock);
	//Start from the residual matrix and construct the whole one
	MATRIX ret;
	if (!matrix.isNull())
		ret=matrix->matrix;
	ret.scale(sx,sy);
	if (includeRotation && !std::isnan(rotation))
		ret.rotate(rotation*M_PI/180.0);
	ret.translate(tx,ty);
	return ret;
}

bool DisplayObject::isConstructed() const
{
	return ACQUIRE_READ(constructed);
}

void DisplayObject::extractValuesFromMatrix()
{
	//Extract the base components from the matrix and leave in
	//it only the residual components
	assert(!matrix.isNull());
	tx=matrix->matrix.getTranslateX();
	ty=matrix->matrix.getTranslateY();
	sx=matrix->matrix.getScaleX();
	sy=matrix->matrix.getScaleY();
	rotation=matrix->matrix.getRotation();
	//Deapply translation
	matrix->matrix.translate(-tx,-ty);
	//Deapply rotation
	matrix->matrix.rotate(-rotation*M_PI/180.0);
	//Deapply scaling
	matrix->matrix.scale(1.0/sx,1.0/sy);
}

bool DisplayObject::skipRender() const
{
	return !isMask() && !ClipDepth && (visible==false || clippedAlpha()==0.0);
}

bool DisplayObject::defaultRender(RenderContext& ctxt)
{
	const Transform2D& t = ctxt.transformStack().transform();
	const CachedSurface& surface=ctxt.getCachedSurface(this);
	/* surface is only modified from within the render thread
	 * so we need no locking here */
	if(!surface.isValid || !surface.isInitialized || !surface.tex || !surface.tex->isValid())
		return true;
	if (surface.tex->width == 0 || surface.tex->height == 0)
		return true;

	Rectangle* r = this->scalingGrid.getPtr();
	if (!r && getParent())
		r = getParent()->scalingGrid.getPtr();
	ctxt.lsglLoadIdentity();
	ColorTransformBase ct = t.colorTransform;
	MATRIX m = t.matrix;
	ctxt.renderTextured(*surface.tex, surface.getState()->alpha, RenderContext::RGB_MODE,
			ct, false,0.0,RGB(),surface.getState()->smoothing,m,r,t.blendmode);
	return false;
}

void DisplayObject::computeBoundsForTransformedRect(number_t xmin, number_t xmax, number_t ymin, number_t ymax,
		number_t& outXMin, number_t& outYMin, number_t& outWidth, number_t& outHeight,
		const MATRIX& m) const
{
	//As the transformation is arbitrary we have to check all the four vertices
	number_t coords[8];
	m.multiply2D(xmin,ymin,coords[0],coords[1]);
	m.multiply2D(xmin,ymax,coords[2],coords[3]);
	m.multiply2D(xmax,ymax,coords[4],coords[5]);
	m.multiply2D(xmax,ymin,coords[6],coords[7]);
	//Now find out the minimum and maximum that represent the complete bounding rect
	number_t minx=coords[6];
	number_t maxx=coords[6];
	number_t miny=coords[7];
	number_t maxy=coords[7];
	for(int i=0;i<6;i+=2)
	{
		if(coords[i]<minx)
			minx=coords[i];
		else if(coords[i]>maxx)
			maxx=coords[i];
		if(coords[i+1]<miny)
			miny=coords[i+1];
		else if(coords[i+1]>maxy)
			maxy=coords[i+1];
	}
	outXMin=minx;
	outYMin=miny;
	outWidth = maxx - minx;
	outHeight = maxy - miny;
}

IDrawable* DisplayObject::invalidate(bool smoothing)
{
	//Not supposed to be called
	throw RunTimeException("DisplayObject::invalidate");
}
void DisplayObject::invalidateForRenderToBitmap(RenderDisplayObjectToBitmapContainer* container)
{
	IDrawable* d = this->invalidate(container->smoothing);
	if (d)
	{
		if (getNeedsTextureRecalculation() || !d->isCachedSurfaceUsable(this))
		{
			this->incRef();
			AsyncDrawJob* j = new AsyncDrawJob(d,_MR(this));
			j->execute();
			j->threadAbort(); // avoid addUploadJob in jobFence()
			container->uploads.push_back(j);
		}
		else
		{
			RefreshableSurface s;
			this->incRef();
			s.displayobject = _MR(this);
			s.drawable = d;
			container->surfacesToRefresh.push_back(s);
		}
	}
}

void DisplayObject::requestInvalidation(InvalidateQueue* q, bool forceTextureRefresh)
{
	//Let's invalidate also the mask
	if(!mask.isNull())
		mask->requestInvalidation(q);
}

void DisplayObject::updateCachedSurface(IDrawable *d)
{
	// this is called only from rendering thread, so no locking done here
	cachedSurface.SetState(d->getState());
	cachedSurface.isValid=true;
	cachedSurface.isInitialized=true;
	cachedSurface.wasUpdated=true;
}
//TODO: Fix precision issues, Adobe seems to do the matrix mult with twips and rounds the results, 
//this way they have less pb with precision.
void DisplayObject::localToGlobal(number_t xin, number_t yin, number_t& xout, number_t& yout) const
{
	if (this == getSystemState()->mainClip)
	{
		// don't compute current scaling of the main clip into coordinates
		xout=xin;
		yout=yin;
		xout += tx;
		yout += ty;
	}
	else
	{
		getMatrix().multiply2D(xin, yin, xout, yout);
		if(parent)
			parent->localToGlobal(xout, yout, xout, yout);
	}
}
//TODO: Fix precision issues
void DisplayObject::globalToLocal(number_t xin, number_t yin, number_t& xout, number_t& yout) const
{
	getConcatenatedMatrix().getInverted().multiply2D(xin, yin, xout, yout);
}
void DisplayObject::setOnStage(bool staged, bool force,bool inskipping)
{
	bool changed = false;
	//TODO: When removing from stage released the cachedTex
	if(staged!=onStage)
	{
		//Our stage condition changed, send event
		onStage=staged;
		if(staged==true)
		{
			hasChanged=true;
			requestInvalidation(getSystemState());
		}
		if(getVm(getSystemState())==nullptr)
			return;
		changed = true;
		this->requestInvalidationFilterParent();
	}
	if (force || changed)
	{
		/*NOTE: By tests we can assert that added/addedToStage is dispatched
		  immediately when addChild is called. On the other hand setOnStage may
		  be also called outside of the VM thread (for example by Loader::execute)
		  so we have to check isVmThread and act accordingly. If in the future
		  asynchronous uses of setOnStage are removed the code can be simplified
		  by removing the !isVmThread case.
		*/
		if(onStage==true && isConstructed())
		{
			_R<Event> e=_MR(Class<Event>::getInstanceS(getInstanceWorker(),"addedToStage"));
			// the main clip is added to stage after the builtin MovieClip is constructed, but before the constructor call is completed.
			// So the EventListeners for "addedToStage" may not be registered yet and we can't execute the event directly
			if(isVmThread()	&& this != getSystemState()->mainClip) 
				ABCVm::publicHandleEvent(this,e);
			else
			{
				this->incRef();
				getVm(getSystemState())->addEvent(_MR(this),e);
			}
		}
		else if(onStage==false)
		{
			_R<Event> e=_MR(Class<Event>::getInstanceS(getInstanceWorker(),"removedFromStage"));
			if(isVmThread())
				ABCVm::publicHandleEvent(this,e);
			else
			{
				this->incRef();
				getVm(getSystemState())->addEvent(_MR(this),e);
			}
			if (this->is<InteractiveObject>())
				getSystemState()->getEngineData()->InteractiveObjectRemovedFromStage();
			getSystemState()->stage->AVM1RemoveDisplayObject(this);
		}
	}
}

bool DisplayObject::isVisible() const
{
	if (visible)
		return parent ? parent->isVisible() : true;
	return visible;
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setAlpha)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	number_t val;
	ARG_CHECK(ARG_UNPACK (val));
	if (!th->loadedFrom->usesActionScript3) // AVM1 uses alpha values from 0-100
		val /= 100.0;
	
	if (th->colorTransform.isNull())
		th->colorTransform = _NR<ColorTransform>(Class<ColorTransform>::getInstanceS(th->getInstanceWorker()));

	/* The stored value is not clipped, _getAlpha will return the
	 * stored value even if it is outside the [0, 1] range. */
	if(th->colorTransform->alphaMultiplier != val)
	{
		th->colorTransform->alphaMultiplier = val;
		th->hasChanged=true;
		if(th->onStage)
			th->requestInvalidation(wrk->getSystemState(),false);
		else
			th->requestInvalidationFilterParent();
		
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getAlpha)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if (th->loadedFrom->usesActionScript3)
		asAtomHandler::setNumber(ret,wrk,!th->colorTransform.isNull() ? th->colorTransform->alphaMultiplier : 1.0);
	else // AVM1 uses alpha values from 0-100
		asAtomHandler::setNumber(ret,wrk,!th->colorTransform.isNull() ? th->colorTransform->alphaMultiplier*100.0 : 100.0);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getMask)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if(th->mask.isNull())
	{
		asAtomHandler::setNull(ret);
		return;
	}

	th->mask->incRef();
	ret = asAtomHandler::fromObject(th->mask.getPtr());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setMask)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	if(asAtomHandler::is<DisplayObject>(args[0]))
	{
		//We received a valid mask object
		DisplayObject* newMask=asAtomHandler::as<DisplayObject>(args[0]);
		newMask->incRef();
		th->setMask(_MR(newMask));
	}
	else
		th->setMask(NullRef);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getScaleX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->sx);
}

void DisplayObject::setScaleX(number_t val)
{
	if (std::isnan(val))
		return;
	//Apply the difference
	if(sx!=val)
	{
		sx=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setScaleX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	th->setScaleX(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getScaleY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->sy);
}

void DisplayObject::setScaleY(number_t val)
{
	if (std::isnan(val))
		return;
	//Apply the difference
	if(sy!=val)
	{
		sy=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setScaleY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	th->setScaleY(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getScaleZ)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->sz);
}

void DisplayObject::setScaleZ(number_t val)
{
	if (std::isnan(val))
		return;
	ROUND_TO_TWIPS(val);
	//Apply the difference
	if(sz!=val)
	{
		sz=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setScaleZ)
{
	LOG(LOG_NOT_IMPLEMENTED,"DisplayObject.scaleZ is set, but not used anywhere");
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	th->setScaleZ(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->tx);
}

void DisplayObject::setX(number_t val)
{
	//Stop using the legacy matrix
	if(useLegacyMatrix)
		useLegacyMatrix=false;
	if (std::isnan(val))
	{
		if (needsActionScript3())
			val = 0;
		else
			return;
	}
	ROUND_TO_TWIPS(val);
	//Apply translation, it's trivial
	if(tx!=val)
	{
		tx=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

void DisplayObject::setY(number_t val)
{
	//Stop using the legacy matrix
	if(useLegacyMatrix)
		useLegacyMatrix=false;
	if (std::isnan(val))
	{
		if (needsActionScript3())
			val = 0;
		else
			return;
	}
	ROUND_TO_TWIPS(val);
	//Apply translation, it's trivial
	if(ty!=val)
	{
		ty=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

void DisplayObject::setZ(number_t val)
{
	LOG(LOG_NOT_IMPLEMENTED,"setting DisplayObject.z has no effect");
	
	//Stop using the legacy matrix
	if(useLegacyMatrix)
		useLegacyMatrix=false;
	if (std::isnan(val))
		return;
	ROUND_TO_TWIPS(val);
	//Apply translation, it's trivial
	if(tz!=val)
	{
		tz=val;
		hasChanged=true;
		geometryChanged();
		if(onStage)
			requestInvalidation(getSystemState());
		else
			requestInvalidationFilterParent();
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	th->setX(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->ty);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	th->setY(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getZ)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->tz);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setZ)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	th->setZ(val);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getBounds)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);

	MATRIX m;
	if (asAtomHandler::is<DisplayObject>(args[0]))
	{
		DisplayObject* target=asAtomHandler::as<DisplayObject>(args[0]);
		//Compute the transformation matrix
		DisplayObject* cur=th;
		while(cur!=nullptr && cur!=target)
		{
			m = cur->getMatrix().multiplyMatrix(m);
			cur=cur->parent;
		}
		if(cur==nullptr)
		{
			//We crawled all the parent chain without finding the target
			//The target is unrelated, compute it's transformation matrix
			const MATRIX& targetMatrix=target->getConcatenatedMatrix();
			//If it's not invertible just use the previous computed one
			if(targetMatrix.isInvertible())
				m = targetMatrix.getInverted().multiplyMatrix(m);
		}
	}

	Rectangle* res=Class<Rectangle>::getInstanceS(wrk);
	number_t x1,x2,y1,y2;
	bool r=th->getBounds(x1,x2,y1,y2, m);
	if(r)
	{
		//Bounds are in the form [XY]{min,max}
		//convert it to rect (x,y,width,height) representation
		res->x=x1;
		res->width=x2-x1;
		res->y=y1;
		res->height=y2-y1;
	}
	else
	{
		res->x=0;
		res->width=0;
		res->y=0;
		res->height=0;
	}
	ret = asAtomHandler::fromObject(res);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getLoaderInfo)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);

	/* According to tests returning root.loaderInfo is the correct
	 * behaviour, even though the documentation states that only
	 * the main class should have non-null loaderInfo. */
	_NR<RootMovieClip> r=th->getRoot();
	if(r.isNull() || r->loaderInfo.isNull())
	{
		asAtomHandler::setNull(ret);
		return;
	}
	
	r->loaderInfo->incRef();
	ret = asAtomHandler::fromObject(r->loaderInfo.getPtr());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getStage)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	_NR<Stage> res=th->getStage();
	if(res.isNull())
	{
		asAtomHandler::setNull(ret);
		return;
	}

	res->incRef();
	ret = asAtomHandler::fromObject(res.getPtr());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getScale9Grid)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if (th->scalingGrid.getPtr())
	{
		th->scalingGrid->incRef();
		ret = asAtomHandler::fromObjectNoPrimitive(th->scalingGrid.getPtr());
	}
	else
		asAtomHandler::setUndefined(ret);
}
ASFUNCTIONBODY_ATOM(DisplayObject,_setScale9Grid)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	ARG_CHECK(ARG_UNPACK(th->scalingGrid));
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getBlendMode)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	tiny_string res;
	switch (th->blendMode)
	{
		case BLENDMODE_LAYER: res = "layer"; break;
		case BLENDMODE_MULTIPLY: res = "multiply"; break;
		case BLENDMODE_SCREEN: res = "screen"; break;
		case BLENDMODE_LIGHTEN: res = "lighten"; break;
		case BLENDMODE_DARKEN: res = "darken"; break;
		case BLENDMODE_DIFFERENCE: res = "difference"; break;
		case BLENDMODE_ADD: res = "add"; break;
		case BLENDMODE_SUBTRACT: res = "subtract"; break;
		case BLENDMODE_INVERT: res = "invert"; break;
		case BLENDMODE_ALPHA: res = "alpha"; break;
		case BLENDMODE_ERASE: res = "erase"; break;
		case BLENDMODE_OVERLAY: res = "overlay"; break;
		case BLENDMODE_HARDLIGHT: res = "hardlight"; break;
		default: res = "normal"; break;
	}

	ret = asAtomHandler::fromString(wrk->getSystemState(),res);
}
ASFUNCTIONBODY_ATOM(DisplayObject,_setBlendMode)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	tiny_string val;
	ARG_CHECK(ARG_UNPACK(val));
	
	AS_BLENDMODE oldblendmode= th->blendMode;
	th->blendMode = BLENDMODE_NORMAL;
	if (val == "add") th->blendMode = BLENDMODE_ADD;
	else if (val == "alpha") th->blendMode = BLENDMODE_ALPHA;
	else if (val == "darken") th->blendMode = BLENDMODE_DARKEN;
	else if (val == "difference") th->blendMode = BLENDMODE_DIFFERENCE;
	else if (val == "erase") th->blendMode = BLENDMODE_ERASE;
	else if (val == "hardlight") th->blendMode = BLENDMODE_HARDLIGHT;
	else if (val == "invert") th->blendMode = BLENDMODE_INVERT;
	else if (val == "layer") th->blendMode = BLENDMODE_LAYER;
	else if (val == "lighten") th->blendMode = BLENDMODE_LIGHTEN;
	else if (val == "multiply") th->blendMode = BLENDMODE_MULTIPLY;
	else if (val == "overlay") th->blendMode = BLENDMODE_OVERLAY;
	else if (val == "screen") th->blendMode = BLENDMODE_SCREEN;
	else if (val == "subtract") th->blendMode = BLENDMODE_SUBTRACT;
	if (oldblendmode != th->blendMode)
	{
		th->hasChanged=true;
		if(th->onStage)
			th->requestInvalidation(wrk->getSystemState());
		else
			th->requestInvalidationFilterParent();
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,localToGlobal)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen == 1);

	Point* pt=asAtomHandler::as<Point>(args[0]);

	number_t tempx, tempy;

	th->localToGlobal(pt->getX(), pt->getY(), tempx, tempy);

	ret = asAtomHandler::fromObject(Class<Point>::getInstanceS(wrk,tempx, tempy));
}

ASFUNCTIONBODY_ATOM(DisplayObject,globalToLocal)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen == 1);

	Point* pt=asAtomHandler::as<Point>(args[0]);

	number_t tempx, tempy;

	th->globalToLocal(pt->getX(), pt->getY(), tempx, tempy);

	ret = asAtomHandler::fromObject(Class<Point>::getInstanceS(wrk,tempx, tempy));
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setRotation)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	//Apply the difference
	if(th->rotation!=val)
	{
		val = fmod(val+180.0, 360.0) - 180.0;
		if (val < -180.0)
			val += 360.0;
		th->rotation=val;
		th->hasChanged=true;
		th->geometryChanged();
		if(th->onStage)
			th->requestInvalidation(wrk->getSystemState());
	}
}

void DisplayObject::setParent(DisplayObjectContainer *p)
{
	Locker locker(spinlock);
	if(parent!=p)
	{
		if (p)
		{
			// mark old parent as dirty
			geometryChanged();
			getSystemState()->removeFromResetParentList(this);
		}
		parent=p;
		hasChanged=true;
		geometryChanged();
		if(onStage && !getSystemState()->isShuttingDown())
			requestInvalidation(getSystemState());
	}
}

void DisplayObject::setScalingGrid()
{
	RECT* r = loadedFrom->ScalingGridsLookup(this->getTagID());
	if (r)
	{
		this->scalingGrid = _MR(Class<Rectangle>::getInstanceS(getInstanceWorker()));
		this->scalingGrid->x=r->Xmin/20.0;
		this->scalingGrid->y=r->Ymin/20.0;
		this->scalingGrid->width=(r->Xmax-r->Xmin)/20.0;
		this->scalingGrid->height=(r->Ymax-r->Ymin)/20.0;
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getParent)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if(!th->parent)
	{
		asAtomHandler::setUndefined(ret);
		return;
	}

	th->parent->incRef();
	ret = asAtomHandler::fromObject(th->parent);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getRoot)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	_NR<DisplayObject> res;
	
	if (th->isLoadedRootObject())
		res = _NR<DisplayObject>(th);
	else if (th->is<Stage>())
		// according to spec, the root of the stage is the stage itself
		res = _NR<DisplayObject>(th);
	else
		res =th->getRoot();
	if(res.isNull())
	{
		asAtomHandler::setUndefined(ret);
		return;
	}
	res->incRef(); // one ref will be removed during destruction of res

	res->incRef();
	ret = asAtomHandler::fromObject(res.getPtr());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getRotation)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->rotation);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setVisible)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	bool newval = asAtomHandler::Boolean_concrete(args[0]);
	if (newval != th->visible)
	{
		th->visible=newval;
		th->requestInvalidation(wrk->getSystemState());
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getVisible)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setBool(ret,th->visible);
}

number_t DisplayObject::computeHeight()
{
	number_t x1,x2,y1,y2;
	bool ret=getBounds(x1,x2,y1,y2,getMatrix(false));

	return (ret)?(y2-y1):0;
}

void DisplayObject::geometryChanged()
{
	if (this->is<DisplayObjectContainer>())
	{
		this->as<DisplayObjectContainer>()->markBoundsRectDirtyChildren();
	}
	DisplayObjectContainer* p = this->getParent();
	while (p)
	{
		p->markBoundsRectDirty();
		p=p->getParent();
	}
}

number_t DisplayObject::computeWidth()
{
	number_t x1,x2,y1,y2;
	bool ret=getBounds(x1,x2,y1,y2,getMatrix(false));

	return (ret)?(x2-x1):0;
}

int DisplayObject::getRawDepth()
{
	return (parent != nullptr) ? parent->findLegacyChildDepth(this) : 0;
}

int DisplayObject::getDepth()
{
	return getRawDepth() + 16384;
}

int DisplayObject::getClipDepth() const
{
	return ClipDepth ? ClipDepth + 16384 : 0;
}

_NR<RootMovieClip> DisplayObject::getRoot()
{
	if(!parent)
		return NullRef;

	return parent->getRoot();
}

_NR<Stage> DisplayObject::getStage()
{
	if(!parent)
		return NullRef;

	return parent->getStage();
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getWidth)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->computeWidth());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setWidth)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	number_t newwidth=asAtomHandler::toNumber(args[0]);
	if (std::isnan(newwidth))
		return;
	if (std::isinf(newwidth) && th->needsActionScript3())
		newwidth = 0;
	ROUND_TO_TWIPS(newwidth);

	number_t xmin,xmax,y1,y2;
	if(!th->boundsRect(xmin,xmax,y1,y2,false))
		return;

	number_t width=xmax-xmin;
	if(width==0) //Cannot scale, nothing to do (See Reference)
		return;
	
	if(width*th->sx!=newwidth) //If the width is changing, calculate new scale
	{
		if(th->useLegacyMatrix)
			th->useLegacyMatrix=false;
		th->setScaleX(newwidth/width);
	}
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getHeight)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->computeHeight());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_setHeight)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	number_t newheight=asAtomHandler::toNumber(args[0]);
	if (std::isnan(newheight))
		return;
	if (std::isinf(newheight) && th->needsActionScript3())
		newheight = 0;
	ROUND_TO_TWIPS(newheight);

	number_t x1,x2,ymin,ymax;
	if(!th->boundsRect(x1,x2,ymin,ymax,false))
		return;

	number_t height=ymax-ymin;
	if(height==0) //Cannot scale, nothing to do (See Reference)
		return;

	if(height*th->sy!=newheight) //If the height is changing, calculate new scale
	{
		if(th->useLegacyMatrix)
			th->useLegacyMatrix=false;
		th->setScaleY(newheight/height);
	}
}

Vector2f DisplayObject::getLocalMousePos()
{
	return getConcatenatedMatrix().getInverted().multiply2D(getSystemState()->getInputThread()->getMousePos());
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getMouseX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->getLocalMousePos().x);
}

ASFUNCTIONBODY_ATOM(DisplayObject,_getMouseY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,th->getLocalMousePos().y);
}

_NR<DisplayObject> DisplayObject::hitTest(const Vector2f& globalPoint, const Vector2f& localPoint, HIT_TYPE type,bool interactiveObjectsOnly)
{
	if((!(visible || type == GENERIC_HIT_INVISIBLE) || !isConstructed()) && !isMask())
		return NullRef;

	//First check if there is any mask on this object, if so the point must be inside the mask to go on
	if(!mask.isNull())
	{
		//Compute the coordinates local to the mask
		const MATRIX& maskMatrix = mask->getConcatenatedMatrix();
		if(!maskMatrix.isInvertible())
		{
			//If the matrix is not invertible the mask as collapsed to zero size
			//If the mask is zero sized then the object is not visible
			return NullRef;
		}
		const auto maskPoint = maskMatrix.getInverted().multiply2D(globalPoint);
		if(mask->hitTest(globalPoint, maskPoint, type,false).isNull())
			return NullRef;
	}

	return hitTestImpl(globalPoint, localPoint, type,interactiveObjectsOnly);
}

/* Display objects have no children in general,
 * so we skip to calling the constructor, if necessary.
 * This is called in vm's thread context */
void DisplayObject::initFrame()
{
	if(!isConstructed() && getClass() && needsActionScript3())
	{
		asAtom o = asAtomHandler::fromObject(this);
		getClass()->handleConstruction(o,nullptr,0,true);

		/*
		 * Legacy objects have their display list properties set on creation, but
		 * the related events must only be sent after the constructor is sent.
		 * This is from "Order of Operations".
		 */
		if(parent)
		{
			_R<Event> e=_MR(Class<Event>::getInstanceS(getInstanceWorker(),"added"));
			ABCVm::publicHandleEvent(this,e);
		}
		if(onStage)
		{
			_R<Event> e=_MR(Class<Event>::getInstanceS(getInstanceWorker(),"addedToStage"));
			ABCVm::publicHandleEvent(this,e);
		}
	}
}

void DisplayObject::executeFrameScript()
{
}

bool DisplayObject::needsActionScript3() const
{
	return this->loadedFrom && this->loadedFrom->usesActionScript3;
}

void DisplayObject::constructionComplete(bool _explicit)
{
	RELEASE_WRITE(constructed,true);
	if (!placedByActionScript && needsActionScript3() && getParent() != nullptr)
	{
		_R<Event> e=_MR(Class<Event>::getInstanceS(getInstanceWorker(),"added"));
		if (isVmThread())
			ABCVm::publicHandleEvent(this, e);
		else
		{
			incRef();
			getVm(getSystemState())->addEvent(_MR(this), e);
		}
		if (isOnStage())
			setOnStage(true, true);
	}
}
void DisplayObject::beforeConstruction(bool _explicit)
{
	skipFrame |= needsActionScript3() && _explicit;
	placedByActionScript |= needsActionScript3() && _explicit;
	if (needsActionScript3() && getParent() == nullptr)
		getSystemState()->stage->addHiddenObject(this);
}
void DisplayObject::afterConstruction(bool _explicit)
{
//	hasChanged=true;
//	needsTextureRecalculation=true;
//	if(onStage)
//		requestInvalidation(getSystemState());
}

void DisplayObject::applyFilters(BitmapContainer* target, BitmapContainer* source, const RECT& sourceRect, number_t xpos, number_t ypos, number_t scalex, number_t scaley)
{
	if (filters)
	{
		for (uint32_t i = 0; i < filters->size(); i++)
		{
			asAtom f = asAtomHandler::invalidAtom;
			filters->at_nocheck(f,i);
			if (asAtomHandler::is<BitmapFilter>(f))
				asAtomHandler::as<BitmapFilter>(f)->applyFilter(target, source, sourceRect, xpos, ypos, scalex, scaley, this);
		}
	}
}
void DisplayObject::renderFilters(RenderContext& ctxt, uint32_t w, uint32_t h)
{
	// rendering of filters currently works as follows:
	// - generate fbo
	// - generate texture with computed width/height of this DisplayObject as window size and set it as color attachment for fbo
	// - render DisplayObject to texture
	// - set texture as "g_tex_filter1" in fragment shader
	// - generate two more textures with computed width/height of this DisplayObject as window size
	// - for every filter
	//   - for every step (blur, dropshadow...)
	//     - set uniforms for step
	//     - set one of the two textures as color attachment for fbo (use first generated texture in first step)
	//     - render to texture 
	//     - swap textures
	//   - render resulting texture to "g_tex_filter2"
	// - remember resulting texture in cachedSurface.cachedFilterTextureID
	
	if (w == 0 || h == 0)
		return;
	EngineData* engineData = getSystemState()->getEngineData();
	if (cachedSurface.cachedFilterTextureID != UINT32_MAX) // remove previously used texture
		getSystemState()->getRenderThread()->addDeletedTexture(cachedSurface.cachedFilterTextureID);
	cachedSurface.cachedFilterTextureID = UINT32_MAX;
	
	// render filter source to texture
	uint32_t filterTextureIDoriginal;
	engineData->exec_glGenTextures(1, &filterTextureIDoriginal);
	uint32_t filterframebuffer = engineData->exec_glGenFramebuffer();
	engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_STANDARD);
	engineData->exec_glBindTexture_GL_TEXTURE_2D(filterTextureIDoriginal);
	engineData->exec_glBindFramebuffer_GL_FRAMEBUFFER(filterframebuffer);
	uint32_t filterrenderbuffer = engineData->exec_glGenRenderbuffer();
	engineData->exec_glBindRenderbuffer_GL_RENDERBUFFER(filterrenderbuffer);
	engineData->exec_glRenderbufferStorage_GL_RENDERBUFFER_GL_STENCIL_INDEX8(w, h);
	engineData->exec_glFramebufferRenderbuffer_GL_FRAMEBUFFER_GL_STENCIL_ATTACHMENT(filterrenderbuffer);
	engineData->exec_glTexParameteri_GL_TEXTURE_2D_GL_TEXTURE_MIN_FILTER_GL_NEAREST();
	engineData->exec_glTexParameteri_GL_TEXTURE_2D_GL_TEXTURE_MAG_FILTER_GL_NEAREST();
	engineData->exec_glFramebufferTexture2D_GL_FRAMEBUFFER(filterTextureIDoriginal);
	engineData->exec_glTexImage2D_GL_TEXTURE_2D_GL_UNSIGNED_BYTE(0, w, h, 0, nullptr,true);
	uint32_t parentframebufferWidth = getSystemState()->getRenderThread()->currentframebufferWidth;
	uint32_t parentframebufferHeight = getSystemState()->getRenderThread()->currentframebufferHeight;
	
	getSystemState()->getRenderThread()->setViewPort(w,h,true);
	engineData->exec_glClearColor(0,0,0,0);
	engineData->exec_glClear(CLEARMASK(CLEARMASK::COLOR|CLEARMASK::DEPTH|CLEARMASK::STENCIL));
	filterstackentry fe;
	fe.filterframebuffer=filterframebuffer;
	fe.filterrenderbuffer=filterrenderbuffer;
	fe.filtertextureID=filterTextureIDoriginal;
	
	number_t bxmin,bxmax,bymin,bymax;
	boundsRect(bxmin,bxmax,bymin,bymax,false);
	Vector2f scale = getSystemState()->getRenderThread()->getScale();
	fe.filterborderx=(-bxmin+this->maxfilterborder)*scale.x;
	fe.filterbordery=(-bymin+this->maxfilterborder)*scale.y;
	getSystemState()->getRenderThread()->filterframebufferstack.push_back(fe);
	bool maskactive = ctxt.isMaskActive();
	if (maskactive)
		ctxt.suspendActiveMask();
	renderImpl(ctxt);
	// bind rendered filter source to g_tex_filter1
	engineData->exec_glBindFramebuffer_GL_FRAMEBUFFER(filterframebuffer);
	engineData->exec_glBindRenderbuffer_GL_RENDERBUFFER(filterrenderbuffer);
	engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_FILTER);
	engineData->exec_glBindTexture_GL_TEXTURE_2D(filterTextureIDoriginal);
	
	// create filter output texture, and bind it to g_tex_filter2
	engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_FILTER_DST);
	uint32_t filterDstTexture;
	engineData->exec_glGenTextures(1, &filterDstTexture);
	engineData->exec_glBindTexture_GL_TEXTURE_2D(filterDstTexture);
	engineData->exec_glTexParameteri_GL_TEXTURE_2D_GL_TEXTURE_MIN_FILTER_GL_NEAREST();
	engineData->exec_glTexParameteri_GL_TEXTURE_2D_GL_TEXTURE_MAG_FILTER_GL_NEAREST();
	engineData->exec_glFramebufferTexture2D_GL_FRAMEBUFFER(filterDstTexture);
	engineData->exec_glTexImage2D_GL_TEXTURE_2D_GL_UNSIGNED_BYTE(0, w, h, 0, nullptr,true);

	// apply all filter steps
	engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_STANDARD);
	uint32_t filterTextureID1;
	uint32_t filterTextureID2;
	engineData->exec_glGenTextures(1, &filterTextureID1);
	engineData->exec_glBindTexture_GL_TEXTURE_2D(filterTextureID1);
	engineData->exec_glTexImage2D_GL_TEXTURE_2D_GL_UNSIGNED_BYTE(0, w, h, 0, nullptr,true);
	engineData->exec_glGenTextures(1, &filterTextureID2);
	engineData->exec_glBindTexture_GL_TEXTURE_2D(filterTextureID2);
	engineData->exec_glTexImage2D_GL_TEXTURE_2D_GL_UNSIGNED_BYTE(0, w, h, 0, nullptr,true);
	getSystemState()->getRenderThread()->setViewPort(w,h,true);
	uint32_t texture1 = filterTextureIDoriginal;
	uint32_t texture2 = filterTextureID2;
	if (!filters.isNull())
	{
		for (uint32_t i = 0; i < filters->size(); i++)
		{
			asAtom f = asAtomHandler::invalidAtom;
			filters->at_nocheck(f,i);
			if (asAtomHandler::is<BitmapFilter>(f))
			{
				float gradientcolors[256*4];
				asAtomHandler::as<BitmapFilter>(f)->getRenderFilterGradientColors(gradientcolors);
				float filterdata[FILTERDATA_MAXSIZE];
				uint32_t step = 0;
				while (true)
				{
					asAtomHandler::as<BitmapFilter>(f)->getRenderFilterArgs(step,filterdata,w,h);
					if (filterdata[0] == 0)
						break;
					step++;
					engineData->exec_glFramebufferTexture2D_GL_FRAMEBUFFER(texture2);
					engineData->exec_glClearColor(0,0,0,0);
					engineData->exec_glClear(CLEARMASK(CLEARMASK::COLOR|CLEARMASK::DEPTH|CLEARMASK::STENCIL));
					getSystemState()->getRenderThread()->renderTextureToFrameBuffer(texture1,w,h,filterdata,gradientcolors,!i,false);
					if (texture1 == filterTextureIDoriginal)
						texture1 = filterTextureID1;
					std::swap(texture1,texture2);
				}
				engineData->exec_glFramebufferTexture2D_GL_FRAMEBUFFER(filterDstTexture);
				engineData->exec_glClearColor(0,0,0,0);
				engineData->exec_glClear(CLEARMASK(CLEARMASK::COLOR|CLEARMASK::DEPTH|CLEARMASK::STENCIL));
				getSystemState()->getRenderThread()->renderTextureToFrameBuffer(texture1,w,h,nullptr,nullptr,false, false);
			}
		}
	}
	getSystemState()->getRenderThread()->filterframebufferstack.pop_back();
	if (getSystemState()->getRenderThread()->filterframebufferstack.empty())
	{
		getSystemState()->getRenderThread()->resetCurrentFrameBuffer();
		if (!getSystemState()->getRenderThread()->getFlipVertical())
			getSystemState()->getRenderThread()->setViewPort(parentframebufferWidth,parentframebufferHeight,false);
		else
			getSystemState()->getRenderThread()->resetViewPort();
		engineData->exec_glActiveTexture_GL_TEXTURE0(SAMPLEPOSITION::SAMPLEPOS_STANDARD);
	}
	else
	{
		filterstackentry feparent = getSystemState()->getRenderThread()->filterframebufferstack.back();
		engineData->exec_glBindFramebuffer_GL_FRAMEBUFFER(feparent.filterframebuffer);
		engineData->exec_glBindRenderbuffer_GL_RENDERBUFFER(feparent.filterrenderbuffer);
		getSystemState()->getRenderThread()->setViewPort(parentframebufferWidth,parentframebufferHeight,true);
	}
	if (maskactive)
		ctxt.resumeActiveMask();
	engineData->exec_glDeleteFramebuffers(1,&filterframebuffer);
	engineData->exec_glDeleteRenderbuffers(1,&filterrenderbuffer);
	cachedSurface.cachedFilterTextureID=texture1;
	engineData->exec_glDeleteTextures(1,&texture2);
	engineData->exec_glDeleteTextures(1,&filterDstTexture);
	if (filters.isNull() || filters->size()==0)
		engineData->exec_glDeleteTextures(1,&filterTextureID1);
	else
		engineData->exec_glDeleteTextures(1,&filterTextureIDoriginal);
}

void DisplayObject::setNeedsTextureRecalculation(bool skippable)
{
	textureRecalculationSkippable=skippable;
	needsTextureRecalculation=true;
	if (!cachedSurface.isChunkOwner)
		cachedSurface.tex=nullptr;
	cachedSurface.isChunkOwner=true;
}

string DisplayObject::toDebugString() const
{
	std::string res = EventDispatcher::toDebugString();
	res += "tag=";
	char buf[100];
	sprintf(buf,"%u pa=%p",getTagID(),getParent());
	res += buf;
	return res;
}
IDrawable* DisplayObject::getFilterDrawable(bool smoothing)
{
	if (!hasFilters())
		return nullptr;
	number_t x,y;
	number_t width,height;
	number_t bxmin,bxmax,bymin,bymax;
	if(!boundsRect(bxmin,bxmax,bymin,bymax,false))
	{
		//No contents, nothing to do
		return nullptr;
	}
	MATRIX matrix = getMatrix();
	
	bool isMask=false;
	MATRIX m;
	m.scale(matrix.getScaleX(),matrix.getScaleY());
	computeBoundsForTransformedRect(bxmin,bxmax,bymin,bymax,x,y,width,height,m);

	if (isnan(width) || isnan(height))
	{
		// on stage with invalid concatenatedMatrix. Create a trash initial texture
		width = 1;
		height = 1;
	}
	if (width >= 8192 || height >= 8192 || (width * height) >= 16777216)
		return nullptr;

	if(width==0 || height==0)
		return nullptr;
	ColorTransformBase ct;
	if (this->colorTransform)
		ct = *this->colorTransform.getPtr();
	
	Rectangle* r = scalingGrid.getPtr();
	if (!r && getParent())
		r = getParent()->scalingGrid.getPtr();
	
	this->resetNeedsTextureRecalculation();
	return new RefreshableDrawable(x, y, ceil(width), ceil(height)
				, matrix.getScaleX(), matrix.getScaleY()
				, isMask, cacheAsBitmap
				, getConcatenatedAlpha()
				, ct, smoothing ? SMOOTH_MODE::SMOOTH_ANTIALIAS:SMOOTH_MODE::SMOOTH_NONE,this->getBlendMode(),matrix);
}

_NR<DisplayObject> DisplayObject::getCachedBitmap() const
{
	return cachedBitmap;
}

bool DisplayObject::findParent(DisplayObject *d) const
{
	if (this == d)
		return true;
	if (!parent)
		return false;
	return parent->findParent(d);
}

int DisplayObject::getParentDepth() const
{
	int i;
	DisplayObjectContainer* p;
	for (i = 0, p = parent; p != nullptr; p = p->parent, ++i);
	return i;
}

int DisplayObject::findParentDepth(DisplayObject* d) const
{
	if (this != d)
	{
		int i;
		DisplayObjectContainer* p;
		for (i = 0, p = parent; p != nullptr && p != d; p = p->parent, ++i);
		return i;
	}
	return -1;
}

DisplayObjectContainer* DisplayObject::getAncestor(int depth) const
{
	if (depth < 0)
		return (DisplayObjectContainer*)this;
	if (!depth)
		return parent;
	if (parent == nullptr)
		return nullptr;
	return parent->getAncestor(--depth);
}

DisplayObjectContainer* DisplayObject::findCommonAncestor(DisplayObject* d, int& depth, bool init) const
{
	const DisplayObject* a = this;
	const DisplayObject* b = d;
	if (init)
	{
		depth = 0;
		if ((a->getParentDepth() - b->getParentDepth()) < 0)
			std::swap(a, b);
	}
	if (a->parent == nullptr || b == nullptr)
		return a->parent == nullptr ? (DisplayObjectContainer*)a : nullptr;
	if (b->findParent(a->parent))
		return a->parent;
	return a->parent->findCommonAncestor((DisplayObject*)b, ++depth, false);
}

// Compute the minimal, axis aligned bounding box in global
// coordinates
bool DisplayObject::boundsRectGlobal(number_t& xmin, number_t& xmax, number_t& ymin, number_t& ymax)
{
	number_t x1, x2, y1, y2;
	if (!boundsRect(x1, x2, y1, y2,false))
		return false;

	localToGlobal(x1, y1, x1, y1);
	localToGlobal(x2, y2, x2, y2);

	if (!loadedFrom->usesActionScript3 && getRoot())
	{
		// it seems that in AVM1 adobe doesn't use the stage coordinates as reference point, instead it's the root object
		Vector2f rxy =getRoot()->getXY();
		x1-=rxy.x;
		x2-=rxy.x;
		y1-=rxy.y;
		y2-=rxy.y;
	}
	// Mapping to global may swap min and max values (for example,
	// rotation by 180 degrees)
	xmin = dmin(x1, x2);
	xmax = dmax(x1, x2);
	ymin = dmin(y1, y2);
	ymax = dmax(y1, y2);

	return true;
}

ASFUNCTIONBODY_ATOM(DisplayObject,hitTestObject)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	_NR<DisplayObject> another;
	ARG_CHECK(ARG_UNPACK(another));
	number_t xmin = 0, xmax, ymin = 0, ymax;
	if (!th->boundsRectGlobal(xmin, xmax, ymin, ymax))
	{
		asAtomHandler::setBool(ret,false);
		return;
	}

	number_t xmin2, xmax2, ymin2, ymax2;
	if (!another->boundsRectGlobal(xmin2, xmax2, ymin2, ymax2))
	{
		asAtomHandler::setBool(ret,false);
		return;
	}

	number_t intersect_xmax = dmin(xmax, xmax2);
	number_t intersect_xmin = dmax(xmin, xmin2);
	number_t intersect_ymax = dmin(ymax, ymax2);
	number_t intersect_ymin = dmax(ymin, ymin2);
	asAtomHandler::setBool(ret,(intersect_xmax > intersect_xmin) && 
			  (intersect_ymax > intersect_ymin));
}

ASFUNCTIONBODY_ATOM(DisplayObject,hitTestPoint)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	number_t x;
	number_t y;
	bool shapeFlag;
	ARG_CHECK(ARG_UNPACK (x) (y) (shapeFlag, false));

	number_t xmin, xmax, ymin, ymax;
	if (!th->boundsRectGlobal(xmin, xmax, ymin, ymax))
	{
		asAtomHandler::setBool(ret,false);
		return;
	}

	bool insideBoundingBox = (xmin < x) && (x < xmax) && (ymin < y) && (y < ymax);

	if (!shapeFlag)
	{
		asAtomHandler::setBool(ret,insideBoundingBox);
		return;
	}
	else
	{
		if (!insideBoundingBox)
		{
			asAtomHandler::setBool(ret,false);
			return;
		}

		number_t localX;
		number_t localY;
		th->globalToLocal(x, y, localX, localY);
		if (!th->isOnStage())
		{
			// if the DisplayObject is not on stage the local bounds have to be added for hittesting
			localX += xmin;
			localY += ymin;
		}

		// Hmm, hitTest will also check the mask, is this the
		// right thing to do?
		_NR<DisplayObject> hit = th->hitTest(Vector2f(x, y), Vector2f(localX, localY),
						     HIT_TYPE::GENERIC_HIT_INVISIBLE,false);

		asAtomHandler::setBool(ret,!hit.isNull());
	}
}

multiname* DisplayObject::setVariableByMultiname(multiname& name, asAtom& o, CONST_ALLOWED_FLAG allowConst, bool *alreadyset, ASWorker* wrk)
{
	multiname* res = EventDispatcher::setVariableByMultiname(name,o,allowConst,alreadyset,wrk);
	if (!needsActionScript3())
	{
		if (name.name_s_id == BUILTIN_STRINGS::STRING_ONENTERFRAME ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONLOAD)
		{
			if (asAtomHandler::isFunction(o))
			{
				getSystemState()->registerFrameListener(this);
				getSystemState()->stage->AVM1AddEventListener(this);
				avm1framelistenercount++;
				setIsEnumerable(name, false);
			}
			else // value is not a function, we remove the FrameListener
			{
				avm1framelistenercount--;
				if (avm1framelistenercount==0)
					getSystemState()->unregisterFrameListener(this);
			}
		}
		if (this->is<InteractiveObject>() && (
			name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEMOVE ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEDOWN ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEUP ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONPRESS ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEWHEEL ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONROLLOVER ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONROLLOUT ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONRELEASEOUTSIDE ||
			name.name_s_id == BUILTIN_STRINGS::STRING_ONRELEASE))
		{
			this->as<InteractiveObject>()->setMouseEnabled(true);
			getSystemState()->stage->AVM1AddMouseListener(this);
			avm1mouselistenercount++;
			setIsEnumerable(name, false);
		}
	}
	return res;
}
void DisplayObject::AVM1registerPrototypeListeners()
{
	assert(!needsActionScript3());
	ASObject* pr = this->getprop_prototype();
	while (pr)
	{
		multiname name(nullptr);
		name.name_type = multiname::NAME_STRING;
		name.name_s_id = BUILTIN_STRINGS::STRING_ONENTERFRAME;
		if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
		{
			getSystemState()->registerFrameListener(this);
			getSystemState()->stage->AVM1AddEventListener(this);
			avm1framelistenercount++;
		}
		name.name_s_id = BUILTIN_STRINGS::STRING_ONLOAD;
		if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
		{
			getSystemState()->registerFrameListener(this);
			getSystemState()->stage->AVM1AddEventListener(this);
			avm1framelistenercount++;
		}
		if (this->is<InteractiveObject>())
		{
			name.name_s_id = BUILTIN_STRINGS::STRING_ONMOUSEMOVE;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONMOUSEDOWN;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONMOUSEUP;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONPRESS;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONMOUSEWHEEL;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONROLLOVER;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONROLLOUT;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONRELEASEOUTSIDE;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
			name.name_s_id = BUILTIN_STRINGS::STRING_ONRELEASE;
			if (pr->hasPropertyByMultiname(name,true,false,getInstanceWorker()))
			{
				this->as<InteractiveObject>()->setMouseEnabled(true);
				getSystemState()->stage->AVM1AddMouseListener(this);
				avm1mouselistenercount++;
			}
		}
		pr = pr->getprop_prototype();
	}
}
bool DisplayObject::deleteVariableByMultiname(const multiname& name, ASWorker* wrk)
{
	bool res = EventDispatcher::deleteVariableByMultiname(name,wrk);
	if (!this->loadedFrom->usesActionScript3)
	{
		if (name.name_s_id == BUILTIN_STRINGS::STRING_ONENTERFRAME ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONLOAD)
		{
			avm1framelistenercount--;
			if (avm1framelistenercount==0)
				getSystemState()->unregisterFrameListener(this);
		}
		if (this->is<InteractiveObject>() && (
				name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEMOVE ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEDOWN ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEUP ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONPRESS ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONMOUSEWHEEL ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONROLLOVER ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONROLLOUT ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONRELEASEOUTSIDE ||
				name.name_s_id == BUILTIN_STRINGS::STRING_ONRELEASE))
		{
			this->as<InteractiveObject>()->setMouseEnabled(false);
			avm1mouselistenercount--;
			if (avm1mouselistenercount==0)
				getSystemState()->stage->AVM1RemoveMouseListener(this);
		}
		tiny_string s = name.normalizedNameUnresolved(getSystemState()).lowercase();
		AVM1SetVariable(s,asAtomHandler::undefinedAtom,false);
	}
	return res;
}
void DisplayObject::removeAVM1Listeners()
{
	if (needsActionScript3())
		return;
	getSystemState()->stage->AVM1RemoveMouseListener(this);
	getSystemState()->stage->AVM1RemoveKeyboardListener(this);
	getSystemState()->stage->AVM1RemoveEventListener(this);
	getSystemState()->unregisterFrameListener(this);
	avm1mouselistenercount=0;
	avm1framelistenercount=0;
}

ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getScaleX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setInt(ret,wrk,round(th->sx*100.0));
}

ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_setScaleX)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	th->setScaleX(val/100.0);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getScaleY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setInt(ret,wrk,round(th->sy*100.0));
}

ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_setScaleY)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen==1);
	number_t val=asAtomHandler::toNumber(args[0]);
	//Stop using the legacy matrix
	if(th->useLegacyMatrix)
		th->useLegacyMatrix=false;
	th->setScaleY(val/100.0);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getParent)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	DisplayObject* p = th->parent;
	if(!p || p->is<Stage>())
	{
		asAtomHandler::setUndefined(ret);
		return;
	}
	p->incRef();
	ret = asAtomHandler::fromObject(p);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getRoot)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	th->loadedFrom->incRef();
	ret = asAtomHandler::fromObject(th->loadedFrom);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getURL)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	ret = asAtomHandler::fromString(wrk->getSystemState(),th->loadedFrom->getOrigin().getURL());
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_hitTest)
{
	asAtomHandler::setBool(ret,false);
	if (argslen==1)
	{
		if (!asAtomHandler::is<Undefined>(args[0]))
		{
			if (asAtomHandler::is<DisplayObject>(obj))
			{
				if (asAtomHandler::is<DisplayObject>(args[0]))
				{
					hitTestObject(ret,wrk,obj,args,1);
				}
				else
				{
					tiny_string s = asAtomHandler::toString(args[0],wrk);
					DisplayObject* path = asAtomHandler::as<DisplayObject>(obj)->AVM1GetClipFromPath(s);
					if (path)
					{
						asAtom pathobj = asAtomHandler::fromObject(path);
						hitTestObject(ret,wrk,obj,&pathobj,1);
					}
					else
						LOG(LOG_ERROR,"AVM1_hitTest:clip not found:"<<asAtomHandler::toDebugString(args[0]));
				}
			}
			else
				LOG(LOG_NOT_IMPLEMENTED,"AVM1_hitTest:object is no MovieClip:"<<asAtomHandler::toDebugString(obj));
		}
	}
	else
		hitTestPoint(ret,wrk,obj,args,argslen);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_localToGlobal)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen == 1);

	
	ASObject* pt=asAtomHandler::toObject(args[0],wrk);
	
	asAtom x = asAtomHandler::fromInt(0);
	asAtom y = asAtomHandler::fromInt(0);
	multiname mx(nullptr);
	mx.name_type=multiname::NAME_STRING;
	mx.name_s_id=wrk->getSystemState()->getUniqueStringId("x");
	mx.isAttribute = false;
	pt->getVariableByMultiname(x,mx,GET_VARIABLE_OPTION::NONE,wrk);
	multiname my(nullptr);
	my.name_type=multiname::NAME_STRING;
	my.name_s_id=wrk->getSystemState()->getUniqueStringId("y");
	my.isAttribute = false;
	pt->getVariableByMultiname(y,my,GET_VARIABLE_OPTION::NONE,wrk);

	number_t tempx, tempy;

	th->localToGlobal(asAtomHandler::toNumber(x), asAtomHandler::toNumber(y), tempx, tempy);
	asAtomHandler::setNumber(x,wrk,tempx);
	asAtomHandler::setNumber(y,wrk,tempy);
	pt->setVariableByMultiname(mx,x,CONST_ALLOWED,nullptr,wrk);
	pt->setVariableByMultiname(my,y,CONST_ALLOWED,nullptr,wrk);
}

ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_globalToLocal)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	assert_and_throw(argslen == 1);

	
	ASObject* pt=asAtomHandler::toObject(args[0],wrk);
	
	asAtom x = asAtomHandler::fromInt(0);
	asAtom y = asAtomHandler::fromInt(0);
	multiname mx(nullptr);
	mx.name_type=multiname::NAME_STRING;
	mx.name_s_id=wrk->getSystemState()->getUniqueStringId("x");
	mx.isAttribute = false;
	pt->getVariableByMultiname(x,mx,GET_VARIABLE_OPTION::NONE,wrk);
	multiname my(nullptr);
	my.name_type=multiname::NAME_STRING;
	my.name_s_id=wrk->getSystemState()->getUniqueStringId("y");
	my.isAttribute = false;
	pt->getVariableByMultiname(y,my,GET_VARIABLE_OPTION::NONE,wrk);

	number_t tempx, tempy;

	th->globalToLocal(asAtomHandler::toNumber(x), asAtomHandler::toNumber(y), tempx, tempy);
	asAtomHandler::setNumber(x,wrk,tempx);
	asAtomHandler::setNumber(y,wrk,tempy);
	pt->setVariableByMultiname(mx,x,CONST_ALLOWED,nullptr,wrk);
	pt->setVariableByMultiname(my,y,CONST_ALLOWED,nullptr,wrk);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getBytesLoaded)
{
	if (wrk->getSystemState()->mainClip->loaderInfo)
	{
		asAtomHandler::setUInt(ret,wrk,wrk->getSystemState()->mainClip->loaderInfo->getBytesLoaded());
	}
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getBytesTotal)
{
	if (wrk->getSystemState()->mainClip->loaderInfo)
	{
		asAtomHandler::setUInt(ret,wrk,wrk->getSystemState()->mainClip->loaderInfo->getBytesTotal());
	}
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getQuality)
{
	ret = asAtomHandler::fromString(wrk->getSystemState(),wrk->getSystemState()->stage->quality.uppercase());
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_setQuality)
{
	if (argslen > 0)
		wrk->getSystemState()->stage->quality = asAtomHandler::toString(args[0],wrk).uppercase();
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getAlpha)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	asAtomHandler::setNumber(ret,wrk,!th->colorTransform.isNull() ? th->colorTransform->alphaMultiplier*100.0 : 100.0);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_setAlpha)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	number_t val;
	ARG_CHECK(ARG_UNPACK (val));
	val /= 100.0;

	if (th->colorTransform.isNull())
		th->colorTransform = _NR<ColorTransform>(Class<ColorTransform>::getInstanceS(th->getInstanceWorker()));

	if(th->colorTransform->alphaMultiplier != val)
	{
		th->colorTransform->alphaMultiplier = val;
		th->hasChanged=true;
		if(th->onStage)
			th->requestInvalidation(wrk->getSystemState());
		else
			th->requestInvalidationFilterParent();
	}
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getBounds)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);

	ASObject* o =  Class<ASObject>::getInstanceS(wrk);
	ret = asAtomHandler::fromObject(o);
	DisplayObject* target= th;
	if(argslen>=1) // contrary to spec adobe allows getBounds with zero parameters
	{
		if (asAtomHandler::is<Undefined>(args[0]) || asAtomHandler::is<Null>(args[0]))
			return;
		if (!asAtomHandler::is<DisplayObject>(args[0]))
			LOG(LOG_ERROR,"DisplayObject.getBounds invalid type:"<<asAtomHandler::toDebugString(args[0]));
		assert_and_throw(asAtomHandler::is<DisplayObject>(args[0]));
		target =asAtomHandler::as<DisplayObject>(args[0]);
	}
	
	//Compute the transformation matrix
	MATRIX m;
	DisplayObject* cur=th;
	while(cur!=nullptr && cur!=target)
	{
		m = cur->getMatrix().multiplyMatrix(m);
		cur=cur->parent;
	}
	if(cur==nullptr)
	{
		//We crawled all the parent chain without finding the target
		//The target is unrelated, compute it's transformation matrix
		const MATRIX& targetMatrix=target->getConcatenatedMatrix();
		//If it's not invertible just use the previous computed one
		if(targetMatrix.isInvertible())
			m = targetMatrix.getInverted().multiplyMatrix(m);
	}

	number_t x1=0,x2=0,y1=0,y2=0;
	th->getBounds(x1,x2,y1,y2, m);

	asAtom v=asAtomHandler::invalidAtom;
	multiname name(nullptr);
	name.name_type=multiname::NAME_STRING;
	name.name_s_id=wrk->getSystemState()->getUniqueStringId("xMin");
	v = asAtomHandler::fromNumber(wrk,x1,false);
	o->setVariableByMultiname(name,v,ASObject::CONST_ALLOWED,nullptr,wrk);
	name.name_s_id=wrk->getSystemState()->getUniqueStringId("xMax");
	v = asAtomHandler::fromNumber(wrk,x2,false);
	o->setVariableByMultiname(name,v,ASObject::CONST_ALLOWED,nullptr,wrk);
	name.name_s_id=wrk->getSystemState()->getUniqueStringId("yMin");
	v = asAtomHandler::fromNumber(wrk,y1,false);
	o->setVariableByMultiname(name,v,ASObject::CONST_ALLOWED,nullptr,wrk);
	name.name_s_id=wrk->getSystemState()->getUniqueStringId("yMax");
	v = asAtomHandler::fromNumber(wrk,y2,false);
	o->setVariableByMultiname(name,v,ASObject::CONST_ALLOWED,nullptr,wrk);
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_swapDepths)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	if (argslen < 1)
		throw RunTimeException("AVM1: invalid number of arguments for swapDepths");
	DisplayObject* child1 = th;
	DisplayObject* child2 = nullptr;
	if (asAtomHandler::is<DisplayObject>(args[0]))
		child2 = asAtomHandler::as<DisplayObject>(args[0]);
	else
	{
		if (th->getParent() && th->getParent()->hasLegacyChildAt(asAtomHandler::toInt(args[0])))
			child2 = th->getParent()->getLegacyChildAt(asAtomHandler::toInt(args[0]));
	}
	if (th->getParent() && child1 && child2)
	{
		asAtom newargs[2];
		newargs[0] = asAtomHandler::fromObject(child1);
		newargs[1] = asAtomHandler::fromObject(child2);
		asAtom obj = asAtomHandler::fromObject(th->getParent());
		DisplayObjectContainer::swapChildren(ret,wrk,obj,newargs,2);
	}
}
ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_getDepth)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	int r=0;
	if (th->getParent())
		r=th->getParent()->findLegacyChildDepth(th);
	ret = asAtomHandler::fromInt(r);
}

ASFUNCTIONBODY_ATOM(DisplayObject,AVM1_toString)
{
	DisplayObject* th=asAtomHandler::as<DisplayObject>(obj);
	tiny_string s = th->AVM1GetPath();
	if (s.empty())
		DisplayObject::_toString(ret,wrk,obj,args,argslen);
	else
		ret = asAtomHandler::fromString(wrk->getSystemState(),s);
}

void DisplayObject::AVM1SetupMethods(Class_base* c)
{
	// setup all methods and properties available for MovieClips in AVM1
	
	c->destroyContents();
	c->borrowedVariables.destroyContents();
	c->setDeclaredMethodByQName("_x","",Class<IFunction>::getFunction(c->getSystemState(),_getX),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_x","",Class<IFunction>::getFunction(c->getSystemState(),_setX),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_y","",Class<IFunction>::getFunction(c->getSystemState(),_getY),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_y","",Class<IFunction>::getFunction(c->getSystemState(),_setY),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_visible","",Class<IFunction>::getFunction(c->getSystemState(),_getVisible),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_visible","",Class<IFunction>::getFunction(c->getSystemState(),_setVisible),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_xscale","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getScaleX),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_xscale","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_setScaleX),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_yscale","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getScaleY),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_yscale","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_setScaleY),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_width","",Class<IFunction>::getFunction(c->getSystemState(),_getWidth),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_width","",Class<IFunction>::getFunction(c->getSystemState(),_setWidth),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_height","",Class<IFunction>::getFunction(c->getSystemState(),_getHeight),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_height","",Class<IFunction>::getFunction(c->getSystemState(),_setHeight),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_parent","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getParent),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_root","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getRoot),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_url","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getURL),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("hitTest","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_hitTest),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("localToGlobal","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_localToGlobal),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("globalToLocal","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_globalToLocal),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("getBytesLoaded","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getBytesLoaded),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("getBytesTotal","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getBytesTotal),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("_xmouse","",Class<IFunction>::getFunction(c->getSystemState(),_getMouseX),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_ymouse","",Class<IFunction>::getFunction(c->getSystemState(),_getMouseY),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_quality","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getQuality),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_quality","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_setQuality),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_alpha","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getAlpha),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_alpha","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_setAlpha),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("getBounds","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getBounds),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("swapDepths","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_swapDepths),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("getDepth","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_getDepth),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("setMask","",Class<IFunction>::getFunction(c->getSystemState(),_setMask),NORMAL_METHOD,true);
	c->setDeclaredMethodByQName("transform","",Class<IFunction>::getFunction(c->getSystemState(),_getTransform),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("transform","",Class<IFunction>::getFunction(c->getSystemState(),_setTransform),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("_rotation","",Class<IFunction>::getFunction(c->getSystemState(),_getRotation),GETTER_METHOD,true);
	c->setDeclaredMethodByQName("_rotation","",Class<IFunction>::getFunction(c->getSystemState(),_setRotation),SETTER_METHOD,true);
	c->setDeclaredMethodByQName("toString","",Class<IFunction>::getFunction(c->getSystemState(),AVM1_toString,0,Class<ASString>::getRef(c->getSystemState()).getPtr()),NORMAL_METHOD,true);
	REGISTER_GETTER_SETTER_RESULTTYPE(c,scrollRect,Rectangle);
}
DisplayObject *DisplayObject::AVM1GetClipFromPath(tiny_string &path)
{
	if (path.empty() || path == "this")
		return this;
	if (path =="_root")
	{
		return loadedFrom;
	}
	if (path =="_parent")
	{
		return getParent();
	}
	if (path.startsWith("/"))
	{
		tiny_string newpath = path.substr_bytes(1,path.numBytes()-1);
		MovieClip* root = getRoot().getPtr();
		if (root)
			return root->AVM1GetClipFromPath(newpath);
		LOG(LOG_ERROR,"AVM1: no root movie clip for path:"<<path<<" "<<this->toDebugString());
		return nullptr;
	}
	if (path.startsWith("../"))
	{
		tiny_string newpath = path.substr_bytes(3,path.numBytes()-3);
		if (this->getParent() && this->getParent()->is<MovieClip>())
			return this->getParent()->as<MovieClip>()->AVM1GetClipFromPath(newpath);
		LOG(LOG_ERROR,"AVM1: no parent clip for path:"<<path<<" "<<this->toDebugString());
		return nullptr;
	}
	uint32_t pos = path.find("/");
	tiny_string subpath = (pos == tiny_string::npos) ? path : path.substr_bytes(0,pos);
	if (subpath.empty())
	{
		return nullptr;
	}
	// path "/stage" is mapped to the root movie (?) 
	if (this == getSystemState()->mainClip && subpath == "stage")
		return this;
	uint32_t posdot = subpath.find(".");
	if (posdot != tiny_string::npos)
	{
		tiny_string subdotpath =  subpath.substr_bytes(0,posdot);
		if (subdotpath.empty())
			return nullptr;
		DisplayObject* parent = AVM1GetClipFromPath(subdotpath);
		if (!parent)
			return nullptr;
		tiny_string localname = subpath.substr_bytes(posdot+1,subpath.numBytes()-posdot-1);
		return parent->AVM1GetClipFromPath(localname);
	}
	
	multiname objName(nullptr);
	objName.name_type=multiname::NAME_STRING;
	objName.name_s_id=getSystemState()->getUniqueStringId(subpath);
	objName.ns.emplace_back(getSystemState(),BUILTIN_STRINGS::EMPTY,NAMESPACE);
	asAtom ret=asAtomHandler::invalidAtom;
	getVariableByMultiname(ret,objName,GET_VARIABLE_OPTION::NO_INCREF,getInstanceWorker());
	if (asAtomHandler::is<DisplayObject>(ret))
	{
		if (pos == tiny_string::npos)
			return asAtomHandler::as<DisplayObject>(ret);
		else
		{
			subpath = path.substr_bytes(pos+1,path.numBytes()-pos-1);
			return asAtomHandler::as<DisplayObject>(ret)->AVM1GetClipFromPath(subpath);
		}
	}
	return nullptr;
}

void DisplayObject::AVM1SetVariable(tiny_string &name, asAtom v, bool setMember)
{
	if (name.empty())
		return;
	if (name.startsWith("/"))
	{
		tiny_string newpath = name.substr_bytes(1,name.numBytes()-1);
		MovieClip* root = loadedFrom;
		if (root)
			root->AVM1SetVariable(newpath,v);
		else
			LOG(LOG_ERROR,"AVM1: no root movie clip for name:"<<name<<" "<<this->toDebugString());
		return;
	}
	uint32_t pos = name.find(":");
	if (pos == tiny_string::npos)
	{
		ASATOM_INCREF(v); // ensure value is not destructed during binding
		tiny_string localname = name.lowercase();
		uint32_t nameIdOriginal = getSystemState()->getUniqueStringId(name);
		uint32_t nameId = getSystemState()->getUniqueStringId(localname);
		auto it = avm1variables.find(nameId);
		if (it != avm1variables.end())
		{
			ASObject* o = asAtomHandler::getObject(it->second);
			if (o)
				o->removeStoredMember();
		}
		if (asAtomHandler::isUndefined(v))
			avm1variables.erase(nameId);
		else
		{
			ASObject* o = asAtomHandler::getObject(v);
			if (o)
				o->addStoredMember();
			avm1variables[nameId] = v;
		}
		if (setMember)
		{
			multiname objName(nullptr);
			objName.name_type=multiname::NAME_STRING;
			objName.name_s_id=nameIdOriginal;
			ASObject* o = this;
			while (o && !o->hasPropertyByMultiname(objName,true,true,loadedFrom->getInstanceWorker()))
			{
				o=o->getprop_prototype();
			}
			ASATOM_INCREF(v);
			bool alreadyset;
			if (o)
				o->setVariableByMultiname(objName,v, ASObject::CONST_ALLOWED,&alreadyset,loadedFrom->getInstanceWorker());
			else
				setVariableByMultiname(objName,v, ASObject::CONST_ALLOWED,&alreadyset,loadedFrom->getInstanceWorker());
			if (alreadyset)
				ASATOM_DECREF(v);
		}
		AVM1UpdateVariableBindings(nameId,v);
		ASATOM_DECREF(v);
	}
	else if (pos == 0)
	{
		tiny_string localname = name.substr_bytes(pos+1,name.numBytes()-pos-1).lowercase();
		uint32_t nameId = getSystemState()->getUniqueStringId(localname);
		auto it = avm1variables.find(nameId);
		if (it != avm1variables.end())
		{
			ASObject* o = asAtomHandler::getObject(it->second);
			if (o)
				o->removeStoredMember();
		}
		if (asAtomHandler::isUndefined(v))
			avm1variables.erase(nameId);
		else
		{
			ASObject* o = asAtomHandler::getObject(v);
			if (o)
				o->addStoredMember();
			avm1variables[nameId] = v;
		}
	}
	else
	{
		tiny_string path = name.substr_bytes(0,pos);
		DisplayObject* clip = AVM1GetClipFromPath(path);
		if (clip)
		{
			tiny_string localname = name.substr_bytes(pos+1,name.numBytes()-pos-1);
			clip->AVM1SetVariable(localname,v);
		}
	}
}

void DisplayObject::AVM1SetVariableDirect(uint32_t nameId, asAtom v)
{
	auto it = avm1variables.find(nameId);
	if (it != avm1variables.end())
	{
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			o->removeStoredMember();
	}
	if (asAtomHandler::isUndefined(v))
	{
		avm1variables.erase(nameId);
	}
	else
	{
		ASObject* o = asAtomHandler::getObject(v);
		if (o)
			o->addStoredMember();
		avm1variables[nameId] = v;
	}
}

asAtom DisplayObject::AVM1GetVariable(const tiny_string &name, bool checkrootvars)
{
	uint32_t pos = name.find(":");
	if (pos == tiny_string::npos)
	{
		if (loadedFrom->version > 4 && getSystemState()->avm1global)
		{
			// first check for class names
			asAtom ret=asAtomHandler::invalidAtom;
			multiname m(nullptr);
			m.name_type=multiname::NAME_STRING;
			m.name_s_id=getSystemState()->getUniqueStringId(name);
			m.isAttribute = false;
			getSystemState()->avm1global->getVariableByMultiname(ret,m,GET_VARIABLE_OPTION::NONE,getInstanceWorker());
			if(!asAtomHandler::isInvalid(ret))
				return ret;
		}
		auto it = avm1variables.find(getSystemState()->getUniqueStringId(name.lowercase()));
		if (it != avm1variables.end())
		{
			ASATOM_INCREF(it->second);
			return it->second;
		}
	}
	else if (pos == 0)
	{
		tiny_string localname = name.substr_bytes(pos+1,name.numBytes()-pos-1);
		return AVM1GetVariable(localname.lowercase());
	}
	else
	{
		tiny_string path = name.substr_bytes(0,pos).lowercase();
		DisplayObject* clip = AVM1GetClipFromPath(path);
		if (clip)
		{
			tiny_string localname = name.substr_bytes(pos+1,name.numBytes()-pos-1);
			return clip->AVM1GetVariable(localname.lowercase());
		}
	}
	asAtom ret=asAtomHandler::invalidAtom;

	pos = name.find(".");
	if (pos != tiny_string::npos)
	{
		tiny_string path = name;
		DisplayObject* clip = AVM1GetClipFromPath(path);
		if (clip && clip != this)
		{
			clip->incRef();
			ret = asAtomHandler::fromObjectNoPrimitive(clip);
			return ret;
		}
		path = name.lowercase();
		clip = AVM1GetClipFromPath(path);
		if (clip && clip != this)
		{
			clip->incRef();
			ret = asAtomHandler::fromObjectNoPrimitive(clip);
			return ret;
		}
	}

	if (loadedFrom->version > 4 && checkrootvars)
	{
		if (asAtomHandler::isInvalid(ret))// get Variable from root movie
			ret = loadedFrom->AVM1GetVariable(name,false);
	}
	return ret;
}
void DisplayObject::AVM1UpdateVariableBindings(uint32_t nameID, asAtom& value)
{
	auto it = variablebindings.find(nameID);
	while (it != variablebindings.end() && it->first == nameID)
	{
		ASATOM_INCREF(value); // ensure value is not destructed during binding
		(*it).second->UpdateVariableBinding(value);
		it++;
		ASATOM_DECREF(value);
	}
}
asAtom DisplayObject::getVariableBindingValue(const tiny_string &name)
{
	uint32_t pos = name.find(".");
	asAtom ret=asAtomHandler::invalidAtom;
	if (pos == tiny_string::npos)
	{
		ret = AVM1GetVariable(name);
	}
	else
	{
		tiny_string firstpart = name.substr_bytes(0,pos);
		asAtom obj = AVM1GetVariable(firstpart);
		if (asAtomHandler::isValid(obj))
		{
			tiny_string localname = name.substr_bytes(pos+1,name.numBytes()-pos-1);
			ret = asAtomHandler::toObject(obj,getInstanceWorker())->getVariableBindingValue(localname);
			ASATOM_DECREF(obj);
		}
	}
	return ret;
}
void DisplayObject::setVariableBinding(tiny_string &name, _NR<DisplayObject> obj)
{
	uint32_t key = getSystemState()->getUniqueStringId(name);
	if (obj)
	{
		obj->incRef();
		auto it = variablebindings.lower_bound(key);
		while (it != variablebindings.end() && it->first == key)
		{
			if (it->second == obj)
				return;
			it++;
		}
		variablebindings.insert(std::make_pair(key,obj));
	}
	else
	{
		auto it = variablebindings.find(key);
		if (it != variablebindings.end() && it->first == key)
			variablebindings.erase(it);
	}
}
void DisplayObject::AVM1SetFunction(const tiny_string& name, _NR<AVM1Function> obj)
{
	uint32_t nameID = getSystemState()->getUniqueStringId(name);
	uint32_t nameIDlower = getSystemState()->getUniqueStringId(name.lowercase());
	
	auto it = avm1variables.find(nameIDlower);
	if (it != avm1variables.end())
	{
		if (obj && asAtomHandler::isObject(it->second) && asAtomHandler::getObjectNoCheck(it->second) == obj.getPtr())
			return; // function is already set
		ASObject* o = asAtomHandler::getObject(it->second);
		if (o)
			o->removeStoredMember();
	}
	if (obj)
	{
		asAtom v = asAtomHandler::fromObjectNoPrimitive(obj.getPtr());
		obj->incRef();
		obj->addStoredMember();
		avm1variables[nameIDlower] = v;
		
		multiname objName(nullptr);
		objName.name_type=multiname::NAME_STRING;
		objName.name_s_id=nameID;
		obj->incRef();
		bool alreadyset;
		setVariableByMultiname(objName,v, ASObject::CONST_ALLOWED,&alreadyset,loadedFrom->getInstanceWorker());
		if (alreadyset)
			ASATOM_DECREF(v);
	}
	else
	{
		avm1variables.erase(nameIDlower);
	}
}
AVM1Function* DisplayObject::AVM1GetFunction(uint32_t nameID)
{
	auto it = avm1variables.find(nameID);
	if (it != avm1variables.end() && asAtomHandler::is<AVM1Function>(it->second))
		return asAtomHandler::as<AVM1Function>(it->second);
	return nullptr;
}

