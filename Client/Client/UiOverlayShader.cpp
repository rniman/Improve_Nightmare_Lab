#include "stdafx.h"
#include "UiOverlayShader.h"

D3D12_INPUT_LAYOUT_DESC UiOverlayShader::CreateInputLayout()
{
	D3D12_INPUT_ELEMENT_DESC* inputElements = new D3D12_INPUT_ELEMENT_DESC[3];

	inputElements[0] = {
		"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
	};
	inputElements[1] = {
		"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
	};
	inputElements[2] = {
		"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D12_APPEND_ALIGNED_ELEMENT,
		D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
	};

	return {inputElements, 3};
}

D3D12_RASTERIZER_DESC UiOverlayShader::CreateRasterizerState()
{
	D3D12_RASTERIZER_DESC rasterizerState = {};
	rasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
	rasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	rasterizerState.DepthClipEnable = TRUE;

	return rasterizerState;
}

D3D12_BLEND_DESC UiOverlayShader::CreateBlendState()
{
	D3D12_BLEND_DESC blendState = {};
	D3D12_RENDER_TARGET_BLEND_DESC& renderTarget = blendState.RenderTarget[0];
	renderTarget.BlendEnable = TRUE;
	renderTarget.SrcBlend = D3D12_BLEND_SRC_ALPHA;
	renderTarget.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
	renderTarget.BlendOp = D3D12_BLEND_OP_ADD;
	renderTarget.SrcBlendAlpha = D3D12_BLEND_ONE;
	renderTarget.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
	renderTarget.BlendOpAlpha = D3D12_BLEND_OP_ADD;
	renderTarget.LogicOp = D3D12_LOGIC_OP_NOOP;
	renderTarget.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

	return blendState;
}

D3D12_DEPTH_STENCIL_DESC UiOverlayShader::CreateDepthStencilState()
{
	D3D12_DEPTH_STENCIL_DESC depthStencilState = {};
	depthStencilState.DepthEnable = FALSE;
	depthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
	depthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
	depthStencilState.StencilEnable = FALSE;

	return depthStencilState;
}

D3D12_SHADER_BYTECODE UiOverlayShader::CreateVertexShader()
{
	return ReadCompiledShaderFromFile(L"cso/VSUiOverlay.cso", m_pd3dVertexShaderBlob.GetAddressOf());
}

D3D12_SHADER_BYTECODE UiOverlayShader::CreatePixelShader()
{
	return ReadCompiledShaderFromFile(L"cso/PSUiOverlay.cso", m_pd3dPixelShaderBlob.GetAddressOf());
}

void UiOverlayShader::CreateShader(
	ID3D12Device* device,
	ID3D12GraphicsCommandList* commandList,
	ID3D12RootSignature* rootSignature,
	UINT renderTargetCount,
	DXGI_FORMAT* renderTargetFormats,
	DXGI_FORMAT depthStencilFormat)
{
	m_nPipelineState = 1;
	m_PipeLineIndex = 0;
	m_vpd3dPipelineState.clear();
	m_vpd3dPipelineState.resize(m_nPipelineState);

	CShader::CreateShader(
		device,
		commandList,
		rootSignature,
		renderTargetCount,
		renderTargetFormats,
		depthStencilFormat
	);
}

void UiOverlayShader::PrepareRender(ID3D12GraphicsCommandList* commandList)
{
	UpdatePipeLineState(commandList, 0);
}
