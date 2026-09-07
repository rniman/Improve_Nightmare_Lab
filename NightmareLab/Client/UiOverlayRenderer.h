#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <d3d12.h>
#include <DirectXMath.h>
#include <wrl.h>

#include "UiOverlayShader.h"

class CTexture;
struct UiOverlayElement;
struct UiOverlayFrameData;

enum class UiOverlayTexture
{
	SurvivorStartMessage,
	ZombieObjective,
	CountdownFont,
	DistanceFont,
	Count
};

enum class UiOverlayFont
{
	Countdown,
	Distance,
	Count
};

struct UiGlyphMetric
{
	DirectX::XMFLOAT2 uvMin = {};
	DirectX::XMFLOAT2 uvMax = {};
	DirectX::XMFLOAT2 size = {};
	DirectX::XMFLOAT2 offset = {};
	float advance = 0.0f;
};

struct UiBitmapFont
{
	const UiGlyphMetric* FindGlyph(std::uint32_t codepoint) const;

	std::unordered_map<std::uint32_t, UiGlyphMetric> glyphs;
	DirectX::XMFLOAT2 atlasSize = {};
	float lineHeight = 0.0f;
	float baseline = 0.0f;
};

struct UiOverlayDrawBatch
{
	UiOverlayTexture texture = UiOverlayTexture::SurvivorStartMessage;
	UINT firstVertex = 0;
	UINT vertexCount = 0;
};

class UiOverlayRenderer
{
public:
	UiOverlayRenderer();
	~UiOverlayRenderer();

	UiOverlayRenderer(const UiOverlayRenderer&) = delete;
	UiOverlayRenderer& operator=(const UiOverlayRenderer&) = delete;

	bool Initialize(
		ID3D12Device* device,
		ID3D12GraphicsCommandList* commandList,
		ID3D12RootSignature* rootSignature
	);
	void Shutdown();
	void ReleaseUploadBuffers();
	void BuildFrameGeometry(const UiOverlayFrameData& frameData, const DirectX::XMFLOAT2& viewportSize);
	void Render(ID3D12GraphicsCommandList* commandList, D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView);

	const UiBitmapFont& GetFont(UiOverlayFont font) const;
	ID3D12DescriptorHeap* GetDescriptorHeap() const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetTextureHandle(UiOverlayTexture texture) const;

private:
	bool LoadBitmapFont(
		const std::string& metadataPath,
		const std::string& expectedTextureName,
		UiBitmapFont& font
	);
	bool LoadTextures(ID3D12Device* device,	ID3D12GraphicsCommandList* commandList);
	bool CreateShaderResourceViews(ID3D12Device* device);
	bool CreateVertexBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList);
	void AppendFixedQuad(const UiOverlayElement& element, UiOverlayTexture texture);
	void AppendTextQuads(
		const UiOverlayElement& element,
		const std::string& text,
		UiOverlayFont font,
		UiOverlayTexture texture
	);
	void AppendQuad(
		const DirectX::XMFLOAT2& topLeft,
		const DirectX::XMFLOAT2& bottomRight,
		const DirectX::XMFLOAT2& uvMin,
		const DirectX::XMFLOAT2& uvMax,
		const DirectX::XMFLOAT4& color
	);
	void AddBatch(UiOverlayTexture texture, UINT firstVertex);
	DirectX::XMFLOAT2 ConvertPixelToNdc(const DirectX::XMFLOAT2& position) const;

	static constexpr std::size_t MAX_QUAD_COUNT = 32;
	static constexpr std::size_t VERTICES_PER_QUAD = 6;
	static constexpr std::size_t MAX_VERTEX_COUNT = MAX_QUAD_COUNT * VERTICES_PER_QUAD;

	std::shared_ptr<CTexture> mTextures;
	std::unique_ptr<UiOverlayShader> mShader;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
	Microsoft::WRL::ComPtr<ID3D12Resource> mVertexBuffer;
	UiOverlayVertex* mMappedVertices = nullptr;
	D3D12_VERTEX_BUFFER_VIEW mVertexBufferView = {};
	std::array<UiOverlayVertex, MAX_VERTEX_COUNT> mCpuVertices = {};
	std::array<UiOverlayDrawBatch, static_cast<std::size_t>(UiOverlayTexture::Count)> mDrawBatches = {};
	DirectX::XMFLOAT2 mViewportSize = {};
	UINT mVertexCount = 0;
	UINT mDrawBatchCount = 0;
	std::array<UiBitmapFont, static_cast<std::size_t>(UiOverlayFont::Count)> mFonts;
	std::array<
		D3D12_GPU_DESCRIPTOR_HANDLE,
		static_cast<std::size_t>(UiOverlayTexture::Count)> mTextureHandles = {};
};
