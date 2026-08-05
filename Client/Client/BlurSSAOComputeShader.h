#pragma once
#include "Shader.h"


class CBlurSSAOComputeShader : public CComputeShader
{
public:
	CBlurSSAOComputeShader() = default;
	~CBlurSSAOComputeShader() override = default;

	D3D12_SHADER_BYTECODE CreateComputeShader(ID3DBlob** ppd3dShaderBlob) override;
	void CreateShader(
		ID3D12Device* pd3dDevice,
		ID3D12GraphicsCommandList* pd3dCommandList,
		ID3D12RootSignature* pd3dRootSignature
	);
	void CreateShaderVariables(ID3D12Device* pd3dDevice, ID3D12GraphicsCommandList* pd3dCommandList);

	void Blur(ID3D12GraphicsCommandList* pd3dCommandList, const shared_ptr<CCamera>& pCamera);

	void SetRawAmbientOcclusionTexture(shared_ptr<CTexture>& pTexture) { m_pRawAmbientOcclusionTexture = pTexture; }
	void SetGBufferTexture(shared_ptr<CTexture>& pTexture) { m_pGBufferTexture = pTexture; }
	shared_ptr<CTexture>& GetBlurredAmbientOcclusionTexture() { return m_pVerticalBlurTexture; }

private:
	void DispatchHorizontal(ID3D12GraphicsCommandList* pd3dCommandList);
	void DispatchVertical(ID3D12GraphicsCommandList* pd3dCommandList);

	shared_ptr<CTexture> m_pRawAmbientOcclusionTexture;
	shared_ptr<CTexture> m_pGBufferTexture;
	shared_ptr<CTexture> m_pHorizontalBlurTexture;
	shared_ptr<CTexture> m_pVerticalBlurTexture;
};
