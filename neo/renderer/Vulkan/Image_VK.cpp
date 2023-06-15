/*
===========================================================================

Doom 3 BFG Edition GPL Source Code
Copyright (C) 1993-2012 id Software LLC, a ZeniMax Media company.
Copyright (C) 2013-2018 Robert Beckebans
Copyright (C) 2016-2017 Dustin Land

This file is part of the Doom 3 BFG Edition GPL Source Code ("Doom 3 BFG Edition Source Code").

Doom 3 BFG Edition Source Code is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

Doom 3 BFG Edition Source Code is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Doom 3 BFG Edition Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, the Doom 3 BFG Edition Source Code is also subject to certain additional terms. You should have received a copy of these additional terms immediately following the terms and conditions of the GNU General Public License which accompanied the Doom 3 BFG Edition Source Code.  If not, please request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc., Suite 120, Rockville, Maryland 20850 USA.

===========================================================================
*/
#pragma hdrstop
#include "precompiled.h"

/*
================================================================================================
Contains the Image implementation for Vulkan
================================================================================================
*/

#include "../RenderCommon.h"
#include "Staging_VK.h"

int						idImage::garbageIndex = 0;

extern idCVar r_useSRGB;
#if defined( USE_AMD_ALLOCATOR )
idList< VmaAllocation > idImage::allocationGarbage[ NUM_FRAME_DATA ];
#else
idList< vulkanAllocation_t > idImage::allocationGarbage[ NUM_FRAME_DATA ];
#endif
idList< VkImage >		idImage::imageGarbage[ NUM_FRAME_DATA ];
idList< VkImageView >	idImage::viewGarbage[ NUM_FRAME_DATA ];
idList< VkSampler >		idImage::samplerGarbage[ NUM_FRAME_DATA ];

//GK: Direct Copy from RBDOOM-3-BFG
/*
=============
ChooseSupportedFormat
=============
*/
static VkFormat ChooseSupportedFormat(VkFormat* formats, int numFormats, VkImageTiling tiling, VkFormatFeatureFlags features)
{
	for (int i = 0; i < numFormats; ++i)
	{
		VkFormat format = formats[i];

		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(vkcontext.physicalDevice, format, &props);

		if ((tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) 
			|| (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features))
		{
			return format;
		}
	}

	idLib::FatalError("Failed to find a supported format.");
#ifndef _MSC_VER
	return VK_FORMAT_UNDEFINED;
#endif
}

static VkFormat formats[] =
{
	VK_FORMAT_D32_SFLOAT_S8_UINT,
	VK_FORMAT_D24_UNORM_S8_UINT
};

/*
====================
VK_GetFormatFromTextureFormat
====================
*/
static VkFormat VK_GetFormatFromTextureFormat( const textureFormat_t format )
{
	switch( format )
	{
		case FMT_RGBA8:
			return (r_useSRGB.GetInteger() == 1 || r_useSRGB.GetInteger() == 3) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
		case FMT_XRGB8:
			return (r_useSRGB.GetInteger() == 1 || r_useSRGB.GetInteger() == 3) ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8G8B8_UNORM;
		case FMT_ALPHA:
			return (r_useSRGB.GetInteger() == 1 || r_useSRGB.GetInteger() == 3) ? VK_FORMAT_R8G8B8_SRGB : VK_FORMAT_R8_UNORM;
		case FMT_L8A8:
			return VK_FORMAT_R8G8_UNORM;
		case FMT_LUM8:
			return VK_FORMAT_R8_UNORM;
		case FMT_INT8:
			return VK_FORMAT_R8_UNORM;
		case FMT_DXT1:
			return (r_useSRGB.GetInteger() == 1 || r_useSRGB.GetInteger() == 3) ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
		case FMT_DXT5:
			return (r_useSRGB.GetInteger() == 1 || r_useSRGB.GetInteger() == 3) ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
		case FMT_DEPTH:
			return ChooseSupportedFormat(formats, 3, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
		case FMT_SHADOW_ARRAY:
			return ChooseSupportedFormat(formats, 3, VK_IMAGE_TILING_OPTIMAL, VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
		case FMT_X16:
			return VK_FORMAT_R16_UNORM;
		case FMT_Y16_X16:
			return VK_FORMAT_R16G16_UNORM;
		case FMT_RGB565:
			return VK_FORMAT_R5G6B5_UNORM_PACK16;
			
		// RB begin
		//case FMT_ETC1_RGB8_OES,	// 4 bpp
		//case FMT_SHADOW_ARRAY:	// 32 bpp * 6
		//	return VK_FORMAT_
		
		// we might want to use UNORM instead of SFLOAT
		// however this is intended to be used for the HDR lights buffer which should be allowed to go beyond 1.0
		case FMT_RGBA16F:
			return VK_FORMAT_R16G16B16A16_SFLOAT;
			
		case FMT_RGBA32F:
			return VK_FORMAT_R32G32B32A32_SFLOAT;
			
		case FMT_R32F:
			return VK_FORMAT_R32_SFLOAT;
		// RB end
		
		default:
			return VK_FORMAT_UNDEFINED;
	}
}

/*
====================
VK_GetComponentMappingFromTextureFormat
====================
*/
static VkComponentMapping VK_GetComponentMappingFromTextureFormat( const textureFormat_t format, textureColor_t color )
{
	VkComponentMapping componentMapping =
	{
		VK_COMPONENT_SWIZZLE_ZERO,
		VK_COMPONENT_SWIZZLE_ZERO,
		VK_COMPONENT_SWIZZLE_ZERO,
		VK_COMPONENT_SWIZZLE_ZERO
	};
	
	if( color == CFM_GREEN_ALPHA )
	{
		componentMapping.r = VK_COMPONENT_SWIZZLE_ONE;
		componentMapping.g = VK_COMPONENT_SWIZZLE_ONE;
		componentMapping.b = VK_COMPONENT_SWIZZLE_ONE;
		componentMapping.a = VK_COMPONENT_SWIZZLE_G;
		return componentMapping;
	}
	
	switch( format )
	{
		case FMT_LUM8:
			componentMapping.r = VK_COMPONENT_SWIZZLE_R;
			componentMapping.g = VK_COMPONENT_SWIZZLE_R;
			componentMapping.b = VK_COMPONENT_SWIZZLE_R;
			componentMapping.a = VK_COMPONENT_SWIZZLE_ONE;
			break;
		case FMT_L8A8:
			componentMapping.r = VK_COMPONENT_SWIZZLE_R;
			componentMapping.g = VK_COMPONENT_SWIZZLE_R;
			componentMapping.b = VK_COMPONENT_SWIZZLE_R;
			componentMapping.a = VK_COMPONENT_SWIZZLE_G;
			break;
		case FMT_ALPHA:
			componentMapping.r = VK_COMPONENT_SWIZZLE_ONE;
			componentMapping.g = VK_COMPONENT_SWIZZLE_ONE;
			componentMapping.b = VK_COMPONENT_SWIZZLE_ONE;
			componentMapping.a = VK_COMPONENT_SWIZZLE_R;
			break;
		case FMT_INT8:
			componentMapping.r = VK_COMPONENT_SWIZZLE_R;
			componentMapping.g = VK_COMPONENT_SWIZZLE_R;
			componentMapping.b = VK_COMPONENT_SWIZZLE_R;
			componentMapping.a = VK_COMPONENT_SWIZZLE_R;
			break;
		default:
			componentMapping.r = VK_COMPONENT_SWIZZLE_R;
			componentMapping.g = VK_COMPONENT_SWIZZLE_G;
			componentMapping.b = VK_COMPONENT_SWIZZLE_B;
			componentMapping.a = VK_COMPONENT_SWIZZLE_A;
			break;
	}
	
	return componentMapping;
}

/*
====================
idImage::idImage
====================
*/
idImage::idImage( const char* name ) : imgName( name )
{
	// Vulkan specific
	bIsSwapChainImage = false;
	internalFormat = VK_FORMAT_UNDEFINED;
	image = VK_NULL_HANDLE;
	view = VK_NULL_HANDLE;
	layout = VK_IMAGE_LAYOUT_GENERAL;
	sampler = VK_NULL_HANDLE;
	
	generatorFunction = NULL;
	filter = TF_DEFAULT;
	repeat = TR_REPEAT;
	usage = TD_DEFAULT;
	cubeFiles = CF_2D;
	
	referencedOutsideLevelLoad = false;
	levelLoadReferenced = false;
	defaulted = false;
	sourceFileTime = FILE_NOT_FOUND_TIMESTAMP;
	binaryFileTime = FILE_NOT_FOUND_TIMESTAMP;
	refCount = 0;
}

/*
====================
idImage::~idImage
====================
*/
idImage::~idImage()
{
	if( !bIsSwapChainImage )
	{
		PurgeImage();
	}
}

/*
====================
idImage::IsLoaded
====================
*/
bool idImage::IsLoaded() const
{
	return image != VK_NULL_HANDLE; // TODO_VK maybe do something better than this.
}

/*
====================
idImage::CreateSampler
====================
*/
void idImage::CreateSampler()
{
	VkSamplerCreateInfo createInfo = {};
	createInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
	createInfo.maxAnisotropy = 1.0f;
	createInfo.anisotropyEnable = VK_FALSE;
	createInfo.compareEnable = ( opts.format == FMT_DEPTH );
	createInfo.compareOp = ( opts.format == FMT_DEPTH ) ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_NEVER;
	
	switch( filter )
	{
		case TF_DEFAULT:
		case TF_LINEAR:
			createInfo.minFilter = VK_FILTER_LINEAR;
			createInfo.magFilter = VK_FILTER_LINEAR;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			break;
			
		case TF_NEAREST:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
			break;
			
		// RB:
		case TF_NEAREST_MIPMAP:
			createInfo.minFilter = VK_FILTER_NEAREST;
			createInfo.magFilter = VK_FILTER_NEAREST;
			createInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			break;
			
		default:
			idLib::FatalError( "idImage::CreateSampler: unrecognized texture filter %d", filter );
	}
	
	switch( repeat )
	{
		case TR_REPEAT:
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
			break;
			
		case TR_CLAMP:
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
			break;
			
		case TR_CLAMP_TO_ZERO_ALPHA:
			createInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			break;
			
		case TR_CLAMP_TO_ZERO:
			createInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
			createInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			createInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			createInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			break;
		default:
			idLib::FatalError( "idImage::CreateSampler: unrecognized texture repeat mode %d", repeat );
	}
	
	ID_VK_CHECK( vkCreateSampler( vkcontext.device, &createInfo, NULL, &sampler ) );
}

/*
====================
idImage::EmptyGarbage
====================
*/
void idImage::EmptyGarbage()
{
	garbageIndex = ( garbageIndex + 1 ) % NUM_FRAME_DATA;
	
#if defined( USE_AMD_ALLOCATOR )
	idList< VmaAllocation >& allocationsToFree = allocationGarbage[ garbageIndex ];
#else
	idList< vulkanAllocation_t >& allocationsToFree = allocationGarbage[ garbageIndex ];
#endif
	idList< VkImage >& imagesToFree = imageGarbage[ garbageIndex ];
	idList< VkImageView >& viewsToFree = viewGarbage[ garbageIndex ];
	idList< VkSampler >& samplersToFree = samplerGarbage[ garbageIndex ];
	
#if defined( USE_AMD_ALLOCATOR )
	const int numAllocations = allocationsToFree.Num();
	for( int i = 0; i < numAllocations; ++i )
	{
		vmaDestroyImage( vmaAllocator, imagesToFree[ i ], allocationsToFree[ i ] );
	}
#else
	const int numAllocations = allocationsToFree.Num();
	for( int i = 0; i < numAllocations; ++i )
	{
		vulkanAllocator.Free( allocationsToFree[ i ] );
	}
	
	const int numImages = imagesToFree.Num();
	for( int i = 0; i < numImages; ++i )
	{
		vkDestroyImage( vkcontext.device, imagesToFree[ i ], NULL );
	}
#endif
	
	const int numViews = viewsToFree.Num();
	for( int i = 0; i < numViews; ++i )
	{
		vkDestroyImageView( vkcontext.device, viewsToFree[ i ], NULL );
	}
	
	const int numSamplers = samplersToFree.Num();
	for( int i = 0; i < numSamplers; ++i )
	{
		vkDestroySampler( vkcontext.device, samplersToFree[ i ], NULL );
	}
	
	allocationsToFree.Clear();
	imagesToFree.Clear();
	viewsToFree.Clear();
	samplersToFree.Clear();
}

/*
==============
Bind

Automatically enables 2D mapping or cube mapping if needed
==============
*/
void idImage::Bind()
{
	RENDERLOG_PRINTF( "GL_BindTexture( %s )\n", GetName() );
	
	vkcontext.imageParms[ vkcontext.currentImageParm ] = this;
}

/*
====================
CopyFramebuffer
====================
*/
void idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, bool LDR )
{

}

/*
====================
CopyDepthbuffer
====================
*/
void idImage::CopyDepthbuffer( int x, int y, int imageWidth, int imageHeight )
{

}



/*
========================
idImage::SetPixel
========================
*/
void idImage::SetPixel( int mipLevel, int x, int y, const void* data, int dataSize )
{
	SubImageUpload( mipLevel, x, y, 0, 1, 1, data );
}

/*
========================
idImage::SetTexParameters
========================
*/
void idImage::SetTexParameters()
{

}

/*
====================
idImage::SetSamplerState
====================
*/
void idImage::SetSamplerState( textureFilter_t _filter, textureRepeat_t _repeat )
{

}

/*
========================
idImage::AllocImage

Every image will pass through this function. Allocates all the necessary MipMap levels for the
Image, but doesn't put anything in them.

This should not be done during normal game-play, if you can avoid it.
========================
*/
void idImage::AllocImage()
{
	PurgeImage();
	
	internalFormat = VK_GetFormatFromTextureFormat( opts.format );
	
	// Create Sampler
	CreateSampler();
	
	VkImageUsageFlags usageFlags = VK_IMAGE_USAGE_SAMPLED_BIT;
	if( opts.format == FMT_DEPTH )
	{
		usageFlags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	}
	else
	{
		usageFlags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	}
	
	// Create Image
	VkImageCreateInfo imageCreateInfo = {};
	imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
	imageCreateInfo.flags = ( opts.textureType == TT_CUBIC ) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
	imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
	imageCreateInfo.format = internalFormat;
	imageCreateInfo.extent.width = opts.width;
	imageCreateInfo.extent.height = opts.height;
	imageCreateInfo.extent.depth = 1;
	imageCreateInfo.mipLevels = opts.numLevels;
	imageCreateInfo.arrayLayers = ( opts.textureType == TT_CUBIC || opts.textureType == TT_2D_ARRAY ) ? 6 : 1;
	imageCreateInfo.samples = static_cast< VkSampleCountFlagBits >( opts.samples );
	imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
	imageCreateInfo.usage = usageFlags;
	imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
	
#if defined( USE_AMD_ALLOCATOR )
	VmaMemoryRequirements vmaReq = {};
	vmaReq.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	
	ID_VK_CHECK( vmaCreateImage( vmaAllocator, &imageCreateInfo, &vmaReq, &image, &allocation, NULL ) );
#else
	ID_VK_CHECK( vkCreateImage( vkcontext.device, &imageCreateInfo, NULL, &image ) );
	
	VkMemoryRequirements memoryRequirements;
	vkGetImageMemoryRequirements( vkcontext.device, image, &memoryRequirements );
	
	allocation = vulkanAllocator.Allocate(
					 memoryRequirements.size,
					 memoryRequirements.alignment,
					 memoryRequirements.memoryTypeBits,
					 VULKAN_MEMORY_USAGE_GPU_ONLY,
					 VULKAN_ALLOCATION_TYPE_IMAGE_OPTIMAL );
	
	ID_VK_CHECK( vkBindImageMemory( vkcontext.device, image, allocation.deviceMemory, allocation.offset ) );
#endif
	
	//idLib::Printf( "Vulkan Image alloc '%s': %p\n", GetName(), image );
	
	// Create Image View
	VkImageViewCreateInfo viewCreateInfo = {};
	viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
	viewCreateInfo.image = image;
	viewCreateInfo.viewType = ( opts.textureType == TT_CUBIC ) ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
	viewCreateInfo.format = internalFormat;
	viewCreateInfo.components = VK_GetComponentMappingFromTextureFormat( opts.format, opts.colorFormat );
	viewCreateInfo.subresourceRange.aspectMask = ( opts.format == FMT_DEPTH ) ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
	viewCreateInfo.subresourceRange.levelCount = opts.numLevels;
	viewCreateInfo.subresourceRange.layerCount = ( opts.textureType == TT_CUBIC || opts.textureType == TT_2D_ARRAY ) ? 6 : 1;
	viewCreateInfo.subresourceRange.baseMipLevel = 0;
	
	ID_VK_CHECK( vkCreateImageView( vkcontext.device, &viewCreateInfo, NULL, &view ) );
}

/*
====================
idImage::PurgeImage
====================
*/
void idImage::PurgeImage()
{
	if( sampler != VK_NULL_HANDLE )
	{
		samplerGarbage[ garbageIndex ].Append( sampler );
		sampler = VK_NULL_HANDLE;
	}
	
	if( image != VK_NULL_HANDLE )
	{
		allocationGarbage[ garbageIndex ].Append( allocation );
		viewGarbage[ garbageIndex ].Append( view );
		imageGarbage[ garbageIndex ].Append( image );
		
#if defined( USE_AMD_ALLOCATOR )
		allocation = NULL;
#else
		allocation = vulkanAllocation_t();
#endif
		
		view = VK_NULL_HANDLE;
		image = VK_NULL_HANDLE;
	}
}

/*
========================
idImage::Resize
========================
*/
void idImage::Resize( int width, int height )
{
	if (opts.width == width && opts.height == height)
	{
		return;
	}
	opts.width = width;
	opts.height = height;
	AllocImage();
}

/*
====================
idImage::SubImageUpload
====================
*/
void idImage::SubImageUpload( int mipLevel, int x, int y, int z, int width, int height, const void* pic, int pixelPitch )
{
	assert( x >= 0 && y >= 0 && mipLevel >= 0 && width >= 0 && height >= 0 && mipLevel < opts.numLevels );
	
	// SRS - Calculate buffer size without changing original width and height dimensions for compressed images
	int bufferW = width;
	int bufferH = height;

	if (IsCompressed())
	{
		bufferW = (width + 3) & ~3;
		bufferH = (height + 3) & ~3;
	}

	int size = bufferW * bufferH * BitsForFormat(opts.format) / 8;
	// SRS end
	
	VkBuffer buffer;
	VkCommandBuffer commandBuffer;
	int offset = 0;
	byte* data = stagingManager.Stage( size, 16, commandBuffer, buffer, offset );
	if( opts.format == FMT_RGB565 )
	{
		byte* imgData = ( byte* )pic;
		for( int i = 0; i < size; i += 2 )
		{
			data[ i ] = imgData[ i + 1 ];
			data[ i + 1 ] = imgData[ i ];
		}
	}
	else
	{
		memcpy( data, pic, size );
	}
	
	VkBufferImageCopy imgCopy = {};
	imgCopy.bufferOffset = offset;
	imgCopy.bufferRowLength = pixelPitch;
	imgCopy.bufferImageHeight = bufferH;
	imgCopy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	imgCopy.imageSubresource.layerCount = 1;
	imgCopy.imageSubresource.mipLevel = mipLevel;
	imgCopy.imageSubresource.baseArrayLayer = z;
	imgCopy.imageOffset.x = x;
	imgCopy.imageOffset.y = y;
	imgCopy.imageOffset.z = 0;
	imgCopy.imageExtent.width = width;
	imgCopy.imageExtent.height = height;
	imgCopy.imageExtent.depth = 1;
	
	VkImageMemoryBarrier barrier = {};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.baseMipLevel = 0;
	barrier.subresourceRange.levelCount = opts.numLevels;
	barrier.subresourceRange.baseArrayLayer = z;
	barrier.subresourceRange.layerCount = 1;
	
	barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.srcAccessMask = 0;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
	
	vkCmdCopyBufferToImage( commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imgCopy );
	
	barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier( commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 1, &barrier );
	
	layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}