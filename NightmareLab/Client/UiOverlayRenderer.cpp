#include "stdafx.h"
#include "UiOverlayRenderer.h"

#include "Object.h"
#include "UiOverlayData.h"
#include "UiOverlayShader.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace
{
	constexpr std::size_t TEXTURE_COUNT = static_cast<std::size_t>(UiOverlayTexture::Count);
	constexpr UINT OVERLAY_TEXTURE_ROOT_PARAMETER = 3;

	bool TryReadInteger(std::string_view line, std::string_view key, int& value)
	{
		const std::size_t keyPosition = line.find(key);
		if (keyPosition == std::string_view::npos)
		{
			return false;
		}

		const char* begin = line.data() + keyPosition + key.size();
		const char* end = line.data() + line.size();
		const std::from_chars_result result = std::from_chars(begin, end, value);
		return result.ec == std::errc();
	}

	bool TryReadQuotedString(std::string_view line, std::string_view key, std::string& value)
	{
		const std::size_t keyPosition = line.find(key);
		if (keyPosition == std::string_view::npos)
		{
			return false;
		}

		const std::size_t quoteBegin = line.find('"', keyPosition + key.size());
		if (quoteBegin == std::string_view::npos)
		{
			return false;
		}

		const std::size_t quoteEnd = line.find('"', quoteBegin + 1);
		if (quoteEnd == std::string_view::npos)
		{
			return false;
		}

		value.assign(line.substr(quoteBegin + 1, quoteEnd - quoteBegin - 1));
		return true;
	}
}

const UiGlyphMetric* UiBitmapFont::FindGlyph(std::uint32_t codepoint) const
{
	const auto glyph = glyphs.find(codepoint);
	return glyph == glyphs.end() ? nullptr : &glyph->second;
}

UiOverlayRenderer::UiOverlayRenderer() = default;

UiOverlayRenderer::~UiOverlayRenderer()
{
	Shutdown();
}

bool UiOverlayRenderer::Initialize(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12RootSignature* rootSignature)
{
	Shutdown();
	if (!device || !commandList || !rootSignature)
	{
		return false;
	}

	UiBitmapFont countdownFont;
	if (!LoadBitmapFont(
		"Asset/Fonts/countdown.fnt",
		"countdown.dds",
		countdownFont
	))
	{
		return false;
	}

	UiBitmapFont distanceFont;
	if (!LoadBitmapFont(
		"Asset/Fonts/distance.fnt",
		"distance.dds",
		distanceFont
	))
	{
		return false;
	}
	for (std::uint32_t digit = '0'; digit <= '9'; ++digit)
	{
		if (!countdownFont.FindGlyph(digit) || !distanceFont.FindGlyph(digit))
		{
			return false;
		}
	}
	if (!distanceFont.FindGlyph('m'))
	{
		return false;
	}

	mFonts[static_cast<std::size_t>(UiOverlayFont::Countdown)] = std::move(countdownFont);
	mFonts[static_cast<std::size_t>(UiOverlayFont::Distance)] = std::move(distanceFont);

	if (!LoadTextures(device, commandList) ||
		!CreateShaderResourceViews(device) ||
		!CreateVertexBuffer(device, commandList))
	{
		Shutdown();
		return false;
	}

	mShader = std::make_unique<UiOverlayShader>();
	mShader->CreateShader(device, commandList, rootSignature);

	return true;
}

void UiOverlayRenderer::Shutdown()
{
	if (mVertexBuffer && mMappedVertices)
	{
		mVertexBuffer->Unmap(0, nullptr);
	}
	mMappedVertices = nullptr;
	mVertexBuffer.Reset();
	mVertexBufferView = {};
	mCpuVertices = {};
	mDrawBatches = {};
	mViewportSize = {};
	mVertexCount = 0;
	mDrawBatchCount = 0;
	mShader.reset();
	mTextures.reset();
	mDescriptorHeap.Reset();
	mFonts = {};
	mTextureHandles = {};
}

void UiOverlayRenderer::ReleaseUploadBuffers()
{
	if (mTextures)
	{
		mTextures->ReleaseUploadBuffers();
	}
}

void UiOverlayRenderer::BuildFrameGeometry(
	const UiOverlayFrameData& frameData,
	const DirectX::XMFLOAT2& viewportSize)
{
	mViewportSize = viewportSize;
	mVertexCount = 0;
	mDrawBatchCount = 0;
	mVertexBufferView.SizeInBytes = 0;

	if (!mMappedVertices || viewportSize.x <= 0.0f || viewportSize.y <= 0.0f)
	{
		return;
	}

	const UiOverlayElement& survivorMessage = frameData.GetElement(UiOverlayType::SurvivorStartMessage);
	if (survivorMessage.visible && survivorMessage.color.w > 0.0f)
	{
		AppendFixedQuad(survivorMessage, UiOverlayTexture::SurvivorStartMessage);
	}

	const UiOverlayElement& countdown =	frameData.GetElement(UiOverlayType::ZombieCountdown);
	if (countdown.visible && countdown.color.w > 0.0f)
	{
		const int countdownValue = countdown.value < 0 ? 0 : countdown.value;
		AppendTextQuads(
			countdown,
			std::to_string(countdownValue),
			UiOverlayFont::Countdown,
			UiOverlayTexture::CountdownFont
		);
	}

	const UiOverlayElement& zombieObjective = frameData.GetElement(UiOverlayType::ZombieObjective);
	if (zombieObjective.visible && zombieObjective.color.w > 0.0f)
	{
		AppendFixedQuad(zombieObjective, UiOverlayTexture::ZombieObjective);
	}

	const UiOverlayElement& radarDistance = frameData.GetElement(UiOverlayType::RadarDistance);
	if (radarDistance.visible && radarDistance.color.w > 0.0f)
	{
		const int distanceValue = radarDistance.value < 0 ? 0 : radarDistance.value;
		AppendTextQuads(
			radarDistance,
			std::to_string(distanceValue) + "m",
			UiOverlayFont::Distance,
			UiOverlayTexture::DistanceFont
		);
	}

	if (mVertexCount == 0)
	{
		return;
	}

	const std::size_t vertexBytes = sizeof(UiOverlayVertex) * mVertexCount;
	std::memcpy(mMappedVertices, mCpuVertices.data(), vertexBytes);
	mVertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);
}

void UiOverlayRenderer::Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView)
{
	if (!commandList || !mShader || mDrawBatchCount == 0 || mVertexCount == 0)
	{
		return;
	}

	ID3D12DescriptorHeap* descriptorHeaps[] = {mDescriptorHeap.Get()};
	commandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);
	commandList->OMSetRenderTargets(1, &renderTargetView, FALSE, nullptr);
	mShader->PrepareRender(commandList);
	commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	commandList->IASetVertexBuffers(0, 1, &mVertexBufferView);

	for (UINT batchIndex = 0; batchIndex < mDrawBatchCount; ++batchIndex)
	{
		const UiOverlayDrawBatch& batch = mDrawBatches[batchIndex];
		commandList->SetGraphicsRootDescriptorTable(
			OVERLAY_TEXTURE_ROOT_PARAMETER,
			GetTextureHandle(batch.texture)
		);
		commandList->DrawInstanced(batch.vertexCount, 1, batch.firstVertex, 0);
	}
}

const UiBitmapFont& UiOverlayRenderer::GetFont(UiOverlayFont font) const
{
	return mFonts[static_cast<std::size_t>(font)];
}

ID3D12DescriptorHeap* UiOverlayRenderer::GetDescriptorHeap() const
{
	return mDescriptorHeap.Get();
}

D3D12_GPU_DESCRIPTOR_HANDLE UiOverlayRenderer::GetTextureHandle(UiOverlayTexture texture) const
{
	return mTextureHandles[static_cast<std::size_t>(texture)];
}

void UiOverlayRenderer::AppendFixedQuad(
	const UiOverlayElement& element,
	UiOverlayTexture texture)
{
	if (element.layoutSize.x <= 0.0f || element.layoutSize.y <= 0.0f)
	{
		return;
	}

	const UINT firstVertex = mVertexCount;
	const D3D12_RESOURCE_DESC textureDescription =
		mTextures->GetResource(static_cast<int>(texture))->GetDesc();
	const DirectX::XMFLOAT2 textureSize(
		static_cast<float>(textureDescription.Width),
		static_cast<float>(textureDescription.Height)
	);
	const DirectX::XMFLOAT2 layoutTopLeft(
		element.centerPosition.x - element.layoutSize.x * 0.5f,
		element.centerPosition.y - element.layoutSize.y * 0.5f
	);
	const DirectX::XMFLOAT2 textureTopLeft(
		layoutTopLeft.x + (element.layoutSize.x - textureSize.x) * 0.5f,
		layoutTopLeft.y + (element.layoutSize.y - textureSize.y) * 0.5f
	);
	AppendQuad(
		textureTopLeft,
		DirectX::XMFLOAT2(
			textureTopLeft.x + textureSize.x,
			textureTopLeft.y + textureSize.y
		),
		DirectX::XMFLOAT2(0.0f, 0.0f),
		DirectX::XMFLOAT2(1.0f, 1.0f),
		element.color
	);
	AddBatch(texture, firstVertex);
}

void UiOverlayRenderer::AppendTextQuads(
	const UiOverlayElement& element,
	const std::string& text,
	UiOverlayFont fontType,
	UiOverlayTexture texture)
{
	if (text.empty() || element.layoutSize.x <= 0.0f || element.layoutSize.y <= 0.0f)
	{
		return;
	}
	const std::size_t remainingQuadCapacity =
		(MAX_VERTEX_COUNT - mVertexCount) / VERTICES_PER_QUAD;
	if (text.size() > remainingQuadCapacity)
	{
		return;
	}

	const UiBitmapFont& font = GetFont(fontType);
	float textWidth = 0.0f;
	for (const unsigned char character : text)
	{
		const UiGlyphMetric* glyph = font.FindGlyph(character);
		if (!glyph)
		{
			return;
		}
		textWidth += glyph->advance;
	}

	const UINT firstVertex = mVertexCount;
	const DirectX::XMFLOAT2 layoutTopLeft(
		element.centerPosition.x - element.layoutSize.x * 0.5f,
		element.centerPosition.y - element.layoutSize.y * 0.5f
	);
	float penX = layoutTopLeft.x + (element.layoutSize.x - textWidth) * 0.5f;
	const float lineTop =
		layoutTopLeft.y + (element.layoutSize.y - font.lineHeight) * 0.5f;
	for (const unsigned char character : text)
	{
		const UiGlyphMetric& glyph = *font.FindGlyph(character);
		const DirectX::XMFLOAT2 topLeft(
			penX + glyph.offset.x,
			lineTop + glyph.offset.y
		);
		AppendQuad(
			topLeft,
			DirectX::XMFLOAT2(
				topLeft.x + glyph.size.x,
				topLeft.y + glyph.size.y
			),
			glyph.uvMin,
			glyph.uvMax,
			element.color
		);
		penX += glyph.advance;
	}
	AddBatch(texture, firstVertex);
}

void UiOverlayRenderer::AppendQuad(
	const DirectX::XMFLOAT2& topLeft,
	const DirectX::XMFLOAT2& bottomRight,
	const DirectX::XMFLOAT2& uvMin,
	const DirectX::XMFLOAT2& uvMax,
	const DirectX::XMFLOAT4& color)
{
	if (mVertexCount + VERTICES_PER_QUAD > MAX_VERTEX_COUNT)
	{
		return;
	}

	const DirectX::XMFLOAT2 topLeftNdc = ConvertPixelToNdc(topLeft);
	const DirectX::XMFLOAT2 topRightNdc = ConvertPixelToNdc(
		DirectX::XMFLOAT2(bottomRight.x, topLeft.y)
	);
	const DirectX::XMFLOAT2 bottomLeftNdc = ConvertPixelToNdc(
		DirectX::XMFLOAT2(topLeft.x, bottomRight.y)
	);
	const DirectX::XMFLOAT2 bottomRightNdc = ConvertPixelToNdc(bottomRight);

	const std::array<UiOverlayVertex, VERTICES_PER_QUAD> quadVertices = {
		UiOverlayVertex{topLeftNdc, uvMin, color},
		UiOverlayVertex{topRightNdc, DirectX::XMFLOAT2(uvMax.x, uvMin.y), color},
		UiOverlayVertex{bottomLeftNdc, DirectX::XMFLOAT2(uvMin.x, uvMax.y), color},
		UiOverlayVertex{bottomLeftNdc, DirectX::XMFLOAT2(uvMin.x, uvMax.y), color},
		UiOverlayVertex{topRightNdc, DirectX::XMFLOAT2(uvMax.x, uvMin.y), color},
		UiOverlayVertex{bottomRightNdc, uvMax, color},
	};

	std::copy(
		quadVertices.begin(),
		quadVertices.end(),
		mCpuVertices.begin() + mVertexCount
	);
	mVertexCount += static_cast<UINT>(VERTICES_PER_QUAD);
}

void UiOverlayRenderer::AddBatch(UiOverlayTexture texture, UINT firstVertex)
{
	if (mVertexCount == firstVertex || mDrawBatchCount >= mDrawBatches.size())
	{
		return;
	}

	UiOverlayDrawBatch& batch = mDrawBatches[mDrawBatchCount++];
	batch.texture = texture;
	batch.firstVertex = firstVertex;
	batch.vertexCount = mVertexCount - firstVertex;
}

DirectX::XMFLOAT2 UiOverlayRenderer::ConvertPixelToNdc(
	const DirectX::XMFLOAT2& position) const
{
	return DirectX::XMFLOAT2(
		position.x * 2.0f / mViewportSize.x - 1.0f,
		1.0f - position.y * 2.0f / mViewportSize.y
	);
}

bool UiOverlayRenderer::LoadBitmapFont(
	const std::string& metadataPath,
	const std::string& expectedTextureName,
	UiBitmapFont& font)
{
	std::ifstream input(metadataPath);
	if (!input)
	{
		return false;
	}

	std::string line;
	bool hasCommonData = false;
	bool hasExpectedTexture = false;
	while (std::getline(input, line))
	{
		if (line.starts_with("common "))
		{
			int lineHeight = 0;
			int baseline = 0;
			int atlasWidth = 0;
			int atlasHeight = 0;
			hasCommonData =
				TryReadInteger(line, "lineHeight=", lineHeight) &&
				TryReadInteger(line, "base=", baseline) &&
				TryReadInteger(line, "scaleW=", atlasWidth) &&
				TryReadInteger(line, "scaleH=", atlasHeight) &&
				atlasWidth > 0 && atlasHeight > 0;

			if (!hasCommonData)
			{
				return false;
			}

			font.lineHeight = static_cast<float>(lineHeight);
			font.baseline = static_cast<float>(baseline);
			font.atlasSize = DirectX::XMFLOAT2(
				static_cast<float>(atlasWidth),
				static_cast<float>(atlasHeight)
			);
		}
		else if (line.starts_with("page "))
		{
			std::string textureName;
			hasExpectedTexture =
				TryReadQuotedString(line, "file=", textureName) &&
				textureName == expectedTextureName;
		}
		else if (line.starts_with("char "))
		{
			if (!hasCommonData)
			{
				return false;
			}

			int codepoint = 0;
			int x = 0;
			int y = 0;
			int width = 0;
			int height = 0;
			int xOffset = 0;
			int yOffset = 0;
			int advance = 0;
			const bool hasGlyphData =
				TryReadInteger(line, "id=", codepoint) &&
				TryReadInteger(line, "x=", x) &&
				TryReadInteger(line, "y=", y) &&
				TryReadInteger(line, "width=", width) &&
				TryReadInteger(line, "height=", height) &&
				TryReadInteger(line, "xoffset=", xOffset) &&
				TryReadInteger(line, "yoffset=", yOffset) &&
				TryReadInteger(line, "xadvance=", advance);
			if (!hasGlyphData || codepoint < 0 || width < 0 || height < 0)
			{
				return false;
			}

			const float atlasWidth = font.atlasSize.x;
			const float atlasHeight = font.atlasSize.y;
			UiGlyphMetric metric;
			metric.uvMin = DirectX::XMFLOAT2(x / atlasWidth, y / atlasHeight);
			metric.uvMax = DirectX::XMFLOAT2(
				(x + width) / atlasWidth,
				(y + height) / atlasHeight
			);
			metric.size = DirectX::XMFLOAT2(
				static_cast<float>(width),
				static_cast<float>(height)
			);
			metric.offset = DirectX::XMFLOAT2(
				static_cast<float>(xOffset),
				static_cast<float>(yOffset)
			);
			metric.advance = static_cast<float>(advance);
			font.glyphs[static_cast<std::uint32_t>(codepoint)] = metric;
		}
	}

	return hasCommonData && hasExpectedTexture && !font.glyphs.empty();
}

bool UiOverlayRenderer::LoadTextures(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList)
{
	const std::array<const wchar_t*, TEXTURE_COUNT> texturePaths = {
		L"Asset/Textures/bluesuit_start.dds",
		L"Asset/Textures/zombie_start.dds",
		L"Asset/Fonts/countdown.dds",
		L"Asset/Fonts/distance.dds",
	};

	for (const wchar_t* path : texturePaths)
	{
		std::error_code error;
		if (!std::filesystem::exists(path, error) || error)
		{
			return false;
		}
	}

	mTextures = std::make_shared<CTexture>(
		static_cast<int>(TEXTURE_COUNT),
		RESOURCE_TEXTURE2D,
		0,
		0
	);
	for (std::size_t index = 0; index < texturePaths.size(); ++index)
	{
		mTextures->LoadTextureFromDDSFile(
			device,
			commandList,
			const_cast<wchar_t*>(texturePaths[index]),
			RESOURCE_TEXTURE2D,
			static_cast<UINT>(index)
		);
		if (!mTextures->GetResource(static_cast<int>(index)))
		{
			return false;
		}
	}

	const auto hasMatchingAtlasSize = [this](
		UiOverlayTexture texture,
		UiOverlayFont font)
		{
			const D3D12_RESOURCE_DESC description = mTextures->GetResource(
				static_cast<int>(texture)
			)->GetDesc();
			const DirectX::XMFLOAT2 atlasSize = GetFont(font).atlasSize;
			return description.Width == static_cast<UINT64>(atlasSize.x) &&
				description.Height == static_cast<UINT>(atlasSize.y);
		};

	if (!hasMatchingAtlasSize(UiOverlayTexture::CountdownFont, UiOverlayFont::Countdown) ||
		!hasMatchingAtlasSize(UiOverlayTexture::DistanceFont, UiOverlayFont::Distance))
	{
		return false;
	}

	return true;
}

bool UiOverlayRenderer::CreateShaderResourceViews(ID3D12Device* device)
{
	D3D12_DESCRIPTOR_HEAP_DESC heapDescription = {};
	heapDescription.NumDescriptors = static_cast<UINT>(TEXTURE_COUNT);
	heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	if (FAILED(device->CreateDescriptorHeap(&heapDescription, IID_PPV_ARGS(&mDescriptorHeap))))
	{
		return false;
	}

	D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle =
		mDescriptorHeap->GetCPUDescriptorHandleForHeapStart();
	D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle =
		mDescriptorHeap->GetGPUDescriptorHandleForHeapStart();
	const UINT descriptorSize = device->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
	);

	for (std::size_t index = 0; index < TEXTURE_COUNT; ++index)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC viewDescription =
			mTextures->GetShaderResourceViewDesc(static_cast<int>(index));
		device->CreateShaderResourceView(
			mTextures->GetResource(static_cast<int>(index)),
			&viewDescription,
			cpuHandle
		);
		mTextureHandles[index] = gpuHandle;
		cpuHandle.ptr += descriptorSize;
		gpuHandle.ptr += descriptorSize;
	}

	return true;
}

bool UiOverlayRenderer::CreateVertexBuffer(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList)
{
	const UINT bufferSize = static_cast<UINT>(sizeof(UiOverlayVertex) * MAX_VERTEX_COUNT);
	mVertexBuffer.Attach(CreateBufferResource(
		device,
		commandList,
		nullptr,
		bufferSize,
		D3D12_HEAP_TYPE_UPLOAD,
		D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		nullptr
	));
	if (!mVertexBuffer)
	{
		return false;
	}

	D3D12_RANGE readRange = {0, 0};
	if (FAILED(mVertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mMappedVertices))))
	{
		mVertexBuffer.Reset();
		return false;
	}

	mVertexBufferView.BufferLocation = mVertexBuffer->GetGPUVirtualAddress();
	mVertexBufferView.SizeInBytes = 0;
	mVertexBufferView.StrideInBytes = sizeof(UiOverlayVertex);
	return true;
}
