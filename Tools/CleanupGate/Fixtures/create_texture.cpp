// The fixture of 08_rendering/create_texture. The test names the line of every site.
void Declare(RGFrame& frame, IDevice* device, const TextureDesc& desc)
{
    const RGTexture transient = frame.CreateTexture("Transient", desc);
    const RGTexture owned = renderFrame->CreateTexture("Owned", desc);
    TextureHandle texture = device->CreateTexture(desc);
    TextureViewHandle view = device->CreateTextureView(texture);
    TextureHandle other = m_Device.CreateTexture(desc);
}
