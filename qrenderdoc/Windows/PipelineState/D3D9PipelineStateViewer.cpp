/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "D3D9PipelineStateViewer.h"
#include <float.h>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>
#include <QXmlStreamWriter>
#include "Code/QRDUtils.h"
#include "Code/Resources.h"
#include "Widgets/Extended/RDHeaderView.h"
#include "Widgets/Extended/RDLabel.h"
#include "Widgets/Extended/RDTreeWidget.h"
#include "toolwindowmanager/ToolWindowManager.h"
#include "PipelineStateViewer.h"
#include "ui_D3D9PipelineStateViewer.h"

// the API's sampler numbers for the displacement map sampler and the first vertex texture sampler
// (D3DDMAPSAMPLER and D3DVERTEXTEXTURESAMPLER0)
static const uint32_t D3D9DMapSampler = 256;
static const uint32_t D3D9VertexSampler0 = 257;

// a vertex or index buffer row: the buffer, the offset to view it from and the format to view it in
struct D3D9BufferTag
{
  D3D9BufferTag() = default;
  D3D9BufferTag(ResourceId i, uint64_t offs, uint64_t size, QString f)
      : id(i), offset(offs), size(size), format(f)
  {
  }

  ResourceId id;
  uint64_t offset = 0;
  uint64_t size = UINT64_MAX;
  QString format;
};

Q_DECLARE_METATYPE(D3D9BufferTag);

// a shader constants row: the stage's register file for one register set, and the constant block
// of the shader's reflection using it, if there is one
struct D3D9ConstantsTag
{
  D3D9ConstantsTag() = default;
  D3D9ConstantsTag(ShaderStage s, ResourceId i, uint32_t sz, QString f, int32_t b)
      : stage(s), id(i), size(sz), format(f), block(b)
  {
  }

  ShaderStage stage = ShaderStage::Vertex;
  ResourceId id;
  uint32_t size = 0;
  QString format;
  int32_t block = -1;
};

Q_DECLARE_METATYPE(D3D9ConstantsTag);

D3D9PipelineStateViewer::D3D9PipelineStateViewer(ICaptureContext &ctx, PipelineStateViewer &common,
                                                 QWidget *parent)
    : QFrame(parent), ui(new Ui::D3D9PipelineStateViewer), m_Ctx(ctx), m_Common(common)
{
  ui->setupUi(this);

  QWidget *pages[Page_Count] = {ui->IA, ui->VS, ui->RS, ui->PS, ui->OM, ui->States};
  for(int p = 0; p < Page_Count; p++)
  {
    QScrollArea *scroll = new QScrollArea(pages[p]);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);

    QWidget *contents = new QWidget(scroll);
    QVBoxLayout *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    contents->setLayout(layout);
    scroll->setWidget(contents);

    pages[p]->layout()->addWidget(scroll);
    m_PageLayouts[p] = layout;
  }

  //////////////////////////////////////////////////////////////////////////
  // input assembly

  {
    // the topology and its diagram on their own row, left aligned like D3D11's, so the
    // declaration's width doesn't push the diagram to the far side of the page
    QWidget *topoRow = new QWidget(this);
    QHBoxLayout *topoLayout = new QHBoxLayout(topoRow);
    topoLayout->setContentsMargins(2, 2, 2, 2);

    m_Topology = new QLabel(topoRow);
    m_Topology->setFont(Formatter::PreferredFont());

    m_TopologyDiagram = new QLabel(topoRow);
    m_TopologyDiagram->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // scaled down to the row's height when it's set, see setState
    m_TopologyDiagram->setScaledContents(true);

    topoLayout->addWidget(new QLabel(tr("Topology:"), topoRow));
    topoLayout->addWidget(m_Topology);
    topoLayout->addSpacing(8);
    topoLayout->addWidget(m_TopologyDiagram);
    topoLayout->addStretch(1);

    QWidget *declRow = new QWidget(this);
    QHBoxLayout *declLayout = new QHBoxLayout(declRow);
    declLayout->setContentsMargins(2, 2, 2, 2);

    m_Declaration = new RDLabel(declRow);
    m_Declaration->setAutoFillBackground(true);
    m_Declaration->setBackgroundRole(QPalette::ToolTipBase);
    m_Declaration->setForegroundRole(QPalette::ToolTipText);
    m_Declaration->setFont(Formatter::PreferredFont());
    m_Declaration->setTextInteractionFlags(Qt::TextSelectableByMouse);

    declLayout->addWidget(new QLabel(tr("Vertex Declaration:"), declRow));
    declLayout->addWidget(m_Declaration, 1);

    m_PageLayouts[Page_IA]->addWidget(topoRow);
    m_PageLayouts[Page_IA]->addWidget(declRow);
  }

  m_Elements = addTable(Page_IA, tr("Vertex Elements"),
                        {tr("Slot"), tr("Usage"), tr("Index"), tr("Type"), tr("Format"),
                         tr("Stream"), tr("Offset"), tr("Method"), tr("Go")},
                        {1, 3, 1, 2, 3, 1, 1, 2, -1}, true);
  m_Streams = addTable(Page_IA, tr("Vertex Streams and Index Buffer"),
                       {tr("Stream"), tr("Buffer"), tr("Stride"), tr("Offset"), tr("Frequency"),
                        tr("Byte Length"), tr("Go")},
                       {1, 4, 1, 1, 3, 2, -1}, true);

  QObject::connect(m_Elements, &RDTreeWidget::itemActivated, this,
                   [this](RDTreeWidgetItem *, int) { on_meshView_clicked(); });

  //////////////////////////////////////////////////////////////////////////
  // shaders

  setupShaderPage(Page_VS, m_Shaders[0]);
  setupShaderPage(Page_PS, m_Shaders[1]);

  // fixed function vertex processing goes with the vertex shader, the texture stages with the
  // pixel shader
  m_Transforms = addTable(Page_VS, tr("Fixed Function: Transforms"),
                          {tr("Transform"), tr("Row 0"), tr("Row 1"), tr("Row 2"), tr("Row 3")},
                          {2, 3, 3, 3, 3}, false);
  m_Lighting = addTable(Page_VS, tr("Fixed Function: Lighting and Material"),
                        {tr("State"), tr("Value")}, {1, 3}, false);
  m_Lights =
      addTable(Page_VS, tr("Fixed Function: Lights"),
               {tr("Index"), tr("Enabled"), tr("Type"), tr("Diffuse"), tr("Specular"), tr("Ambient"),
                tr("Position"), tr("Direction"), tr("Range"), tr("Attenuation"), tr("Spot")},
               {1, 1, 2, 3, 3, 3, 3, 3, 1, 3, 3}, false);

  m_TextureStages =
      addTable(Page_PS, tr("Fixed Function: Texture Stages"),
               {tr("Stage"), tr("Color Op"), tr("Color Arg 1"), tr("Color Arg 2"),
                tr("Color Arg 0"), tr("Alpha Op"), tr("Alpha Arg 1"), tr("Alpha Arg 2"),
                tr("Alpha Arg 0"), tr("Result"), tr("TexCoord"), tr("Transform"), tr("Constant")},
               {1, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2}, false);
  m_Fog = addTable(Page_PS, tr("Fog"), {tr("State"), tr("Value")}, {1, 3}, false);

  //////////////////////////////////////////////////////////////////////////
  // rasterizer

  m_RasterState =
      addTable(Page_RS, tr("Rasterizer State"), {tr("State"), tr("Value")}, {1, 3}, false);
  m_Viewport = addTable(Page_RS, tr("Viewport and Scissor"),
                        {tr("Type"), tr("Enabled"), tr("X"), tr("Y"), tr("Width"), tr("Height"),
                         tr("MinZ"), tr("MaxZ")},
                        {1, 1, 1, 1, 1, 1, 1, 1}, false);
  m_ClipPlanes = addTable(Page_RS, tr("User Clip Planes"),
                          {tr("Index"), tr("Enabled"), tr("Plane")}, {1, 1, 4}, false);

  //////////////////////////////////////////////////////////////////////////
  // output merger

  m_Targets = addTable(Page_OM, tr("Render Targets"),
                       {tr("Slot"), tr("Resource"), tr("Type"), tr("Width"), tr("Height"),
                        tr("Mip"), tr("Slice"), tr("Format"), tr("Go")},
                       {2, 4, 2, 1, 1, 1, 1, 3, -1}, true);
  m_Blends = addTable(Page_OM, tr("Blending"),
                      {tr("Slot"), tr("Enabled"), tr("Col Src"), tr("Col Dst"), tr("Col Op"),
                       tr("Alpha Src"), tr("Alpha Dst"), tr("Alpha Op"), tr("Write Mask")},
                      {1, 1, 2, 2, 2, 2, 2, 2, 1}, false);
  m_OutputState = addTable(Page_OM, tr("Output State"), {tr("State"), tr("Value")}, {1, 3}, false);
  m_DepthState =
      addTable(Page_OM, tr("Depth and Stencil State"), {tr("State"), tr("Value")}, {1, 3}, false);
  m_Stencils = addTable(Page_OM, tr("Stencil Faces"),
                        {tr("Face"), tr("Func"), tr("Fail Op"), tr("Depth Fail Op"), tr("Pass Op")},
                        {1, 2, 2, 2, 2}, false);

  QObject::connect(m_Targets, &RDTreeWidget::itemActivated, this,
                   &D3D9PipelineStateViewer::resource_itemActivated);
  QObject::connect(m_Streams, &RDTreeWidget::itemActivated, this,
                   &D3D9PipelineStateViewer::resource_itemActivated);

  //////////////////////////////////////////////////////////////////////////
  // every render state

  m_RenderStates = addTable(Page_States, tr("Render States"),
                            {tr("Render State"), tr("Value"), tr("As Float")}, {3, 2, 2}, false);

  for(QVBoxLayout *layout : m_PageLayouts)
    layout->addStretch(1);

  // this is often changed just because we're changing some tab in the designer.
  ui->stagesTabs->setCurrentIndex(0);

  ui->stagesTabs->tabBar()->setVisible(false);

  ui->pipeFlow->setStages(
      {
          lit("IA"),
          lit("VS"),
          lit("RS"),
          lit("PS"),
          lit("OM"),
          lit("ST"),
      },
      {
          tr("Input Assembler"),
          tr("Vertex Shader"),
          tr("Rasterizer"),
          tr("Pixel Shader"),
          tr("Output Merger"),
          tr("Render States"),
      });

  ui->pipeFlow->setIsolatedStage(Page_States);

  ui->pipeFlow->setStagesEnabled({true, true, true, true, true, true});

  m_Common.setMeshViewPixmap(ui->meshView);

  // reset everything back to defaults
  clearState();
}

D3D9PipelineStateViewer::~D3D9PipelineStateViewer()
{
  delete ui;
}

RDTreeWidget *D3D9PipelineStateViewer::addTable(Page page, const QString &title,
                                                const QStringList &columns,
                                                const QList<int> &stretch, bool goColumn)
{
  QGroupBox *group = new QGroupBox(title, this);
  QVBoxLayout *layout = new QVBoxLayout(group);
  layout->setContentsMargins(2, 2, 2, 2);

  RDTreeWidget *tree = new RDTreeWidget(group);
  RDHeaderView *header = new RDHeaderView(Qt::Horizontal, this);
  tree->setHeader(header);
  tree->setColumns(columns);
  header->setColumnStretchHints(stretch);
  header->setMinimumSectionSize(40);

  tree->setFont(Formatter::PreferredFont());
  tree->setClearSelectionOnFocusLoss(true);
  tree->setInstantTooltips(true);
  tree->setMinimumHeight(80);

  if(goColumn)
  {
    tree->setHoverIconColumn(columns.count() - 1, Icons::action(), Icons::action_hover());
    m_Common.SetupResourceView(tree);
  }

  layout->addWidget(tree);
  m_PageLayouts[page]->addWidget(group);

  m_Tables.push_back({page, title, columns, tree});

  return tree;
}

void D3D9PipelineStateViewer::setupShaderPage(Page page, ShaderWidgets &widgets)
{
  QWidget *row = new QWidget(this);
  QHBoxLayout *layout = new QHBoxLayout(row);
  layout->setContentsMargins(2, 2, 2, 2);

  widgets.shader = new RDLabel(row);
  widgets.shader->setAutoFillBackground(true);
  widgets.shader->setBackgroundRole(QPalette::ToolTipBase);
  widgets.shader->setForegroundRole(QPalette::ToolTipText);
  widgets.shader->setMinimumSizeHint(QSize(250, 0));
  widgets.shader->setFont(Formatter::PreferredFont());

  widgets.view = new QToolButton(row);
  widgets.view->setText(tr("View"));
  widgets.view->setIcon(Icons::action());
  widgets.view->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  widgets.view->setAutoRaise(true);

  widgets.save = new QToolButton(row);
  widgets.save->setText(tr("Save"));
  widgets.save->setIcon(Icons::save());
  widgets.save->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  widgets.save->setAutoRaise(true);

  layout->addWidget(new QLabel(tr("Shader:"), row));
  layout->addWidget(widgets.shader, 1);
  layout->addWidget(widgets.view);
  layout->addWidget(widgets.save);

  m_PageLayouts[page]->addWidget(row);

  QObject::connect(widgets.view, &QToolButton::clicked, this,
                   &D3D9PipelineStateViewer::shaderView_clicked);
  QObject::connect(widgets.shader, &RDLabel::clicked, this,
                   &D3D9PipelineStateViewer::shaderView_clicked);
  QObject::connect(widgets.save, &QToolButton::clicked, this,
                   &D3D9PipelineStateViewer::shaderSave_clicked);

  widgets.textures = addTable(page, tr("Textures"),
                              {tr("Sampler"), tr("Resource"), tr("Type"), tr("Width"), tr("Height"),
                               tr("Depth"), tr("Mips"), tr("Format"), tr("Go")},
                              {2, 4, 2, 1, 1, 1, 1, 3, -1}, true);
  widgets.samplers = addTable(page, tr("Sampler States"),
                              {tr("Sampler"), tr("Addressing"), tr("Filter"), tr("Max Mip Level"),
                               tr("LOD Bias"), tr("Max Anisotropy"), tr("sRGB"), tr("Border")},
                              {2, 4, 4, 1, 1, 1, 1, 3}, false);
  widgets.constants =
      addTable(page, tr("Constants"),
               {tr("Registers"), tr("Buffer"), tr("Count"), tr("Constant Block"), tr("Go")},
               {2, 4, 1, 3, -1}, true);

  QObject::connect(widgets.textures, &RDTreeWidget::itemActivated, this,
                   &D3D9PipelineStateViewer::resource_itemActivated);
  QObject::connect(widgets.constants, &RDTreeWidget::itemActivated, this,
                   &D3D9PipelineStateViewer::resource_itemActivated);
}

void D3D9PipelineStateViewer::OnCaptureLoaded()
{
  OnEventChanged(m_Ctx.CurEvent());
}

void D3D9PipelineStateViewer::OnCaptureClosed()
{
  ui->pipeFlow->setStagesEnabled({true, true, true, true, true, true});

  clearState();
}

void D3D9PipelineStateViewer::OnEventChanged(uint32_t eventId)
{
  setState();
}

void D3D9PipelineStateViewer::SelectPipelineStage(PipelineStage stage)
{
  switch(stage)
  {
    case PipelineStage::VertexInput: ui->pipeFlow->setSelectedStage(Page_IA); break;
    case PipelineStage::VertexShader: ui->pipeFlow->setSelectedStage(Page_VS); break;
    case PipelineStage::Rasterizer: ui->pipeFlow->setSelectedStage(Page_RS); break;
    case PipelineStage::PixelShader: ui->pipeFlow->setSelectedStage(Page_PS); break;
    case PipelineStage::ColorDepthOutput:
    case PipelineStage::SampleMask: ui->pipeFlow->setSelectedStage(Page_OM); break;
    default: break;
  }
}

ResourceId D3D9PipelineStateViewer::GetResource(RDTreeWidgetItem *item)
{
  if(item == NULL)
    return ResourceId();

  QVariant tag = item->tag();

  if(tag.canConvert<ResourceId>())
    return tag.value<ResourceId>();
  else if(tag.canConvert<D3D9BufferTag>())
    return tag.value<D3D9BufferTag>().id;
  else if(tag.canConvert<D3D9ConstantsTag>())
    return tag.value<D3D9ConstantsTag>().id;

  return ResourceId();
}

void D3D9PipelineStateViewer::on_showUnused_toggled(bool checked)
{
  setState();
}

void D3D9PipelineStateViewer::on_showEmpty_toggled(bool checked)
{
  setState();
}

bool D3D9PipelineStateViewer::showNode(bool usedSlot, bool filledSlot)
{
  const bool showUnused = ui->showUnused->isChecked();
  const bool showEmpty = ui->showEmpty->isChecked();

  // show if it's referenced by the shader - regardless of empty or not
  if(usedSlot)
    return true;

  // it's bound, but not referenced, and we have "show unused"
  if(showUnused && !usedSlot && filledSlot)
    return true;

  // it's empty, and we have "show empty"
  if(showEmpty && !filledSlot)
    return true;

  return false;
}

bool D3D9PipelineStateViewer::isSamplerUsed(ShaderStage stage, uint32_t samplerIndex)
{
  // the descriptor of a sampler is its slot: pixel samplers 0-15, the displacement map 16, vertex
  // samplers 17-20
  uint32_t desc = samplerIndex;
  if(samplerIndex == D3D9DMapSampler)
    desc = 16;
  else if(samplerIndex >= D3D9VertexSampler0)
    desc = 17 + (samplerIndex - D3D9VertexSampler0);

  for(const UsedDescriptor &used : m_Ctx.CurPipelineState().GetReadOnlyResources(stage, true))
    if(used.access.byteOffset == desc)
      return true;

  return false;
}

QString D3D9PipelineStateViewer::resourceName(ResourceId id)
{
  if(id == ResourceId())
    return tr("Empty");
  return m_Ctx.GetResourceName(id);
}

QString D3D9PipelineStateViewer::colorString(const FloatVector &c)
{
  return QFormatStr("%1, %2, %3, %4")
      .arg(Formatter::Format(c.x))
      .arg(Formatter::Format(c.y))
      .arg(Formatter::Format(c.z))
      .arg(Formatter::Format(c.w));
}

QString D3D9PipelineStateViewer::matrixRow(const rdcfixedarray<float, 16> &m, int row)
{
  return QFormatStr("%1, %2, %3, %4")
      .arg(Formatter::Format(m[row * 4 + 0]))
      .arg(Formatter::Format(m[row * 4 + 1]))
      .arg(Formatter::Format(m[row * 4 + 2]))
      .arg(Formatter::Format(m[row * 4 + 3]));
}

void D3D9PipelineStateViewer::clearState()
{
  for(const Table &t : m_Tables)
    t.tree->clear();

  m_Declaration->setText(tr("None"));
  m_Topology->setText(QString());
  m_TopologyDiagram->setPixmap(QPixmap());

  for(ShaderWidgets &w : m_Shaders)
  {
    w.shader->setText(tr("Unbound Shader"));
    w.view->setEnabled(false);
    w.save->setEnabled(false);
  }
}

void D3D9PipelineStateViewer::addTextureRow(RDTreeWidget *tree, const QString &slot,
                                            const D3D9Pipe::Sampler &samp, bool used)
{
  bool filled = samp.resourceId != ResourceId();

  if(!showNode(used, filled))
    return;

  TextureDescription *tex = m_Ctx.GetTexture(samp.resourceId);

  RDTreeWidgetItem *node = NULL;

  if(tex)
  {
    node = new RDTreeWidgetItem({slot, samp.resourceId, ToQStr(tex->type), tex->width, tex->height,
                                 tex->depth, tex->mips, tex->format.Name(), QString()});
  }
  else
  {
    node = new RDTreeWidgetItem({slot, resourceName(samp.resourceId), QString(), QString(),
                                 QString(), QString(), QString(), QString(), QString()});
  }

  node->setTag(QVariant::fromValue(samp.resourceId));

  if(!filled)
  {
    node->setBackgroundColor(QColor(255, 70, 70));
    node->setForegroundColor(QColor(0, 0, 0));
  }
  if(!used)
    node->setItalic(true);

  tree->addTopLevelItem(node);
}

void D3D9PipelineStateViewer::setShaderState(const D3D9Pipe::Shader &sh,
                                             const rdcarray<D3D9Pipe::Sampler> &samplers,
                                             ShaderWidgets &widgets)
{
  const ShaderReflection *refl = sh.reflection;
  const bool vs = sh.stage == ShaderStage::Vertex;

  if(sh.resourceId == ResourceId())
  {
    widgets.shader->setText(vs ? tr("Fixed function vertex processing")
                               : tr("Fixed function texture stages"));
  }
  else
  {
    QString text =
        QFormatStr("%1 (%2)").arg(resourceName(sh.resourceId)).arg(QString(sh.shaderModel));
    if(refl == NULL)
      text += tr(" - no reflection available");
    widgets.shader->setText(text);
  }

  widgets.view->setEnabled(refl != NULL);
  widgets.save->setEnabled(refl != NULL);

  // textures and sampler states
  {
    ScopedTreeUpdater texUpdate(widgets.textures);
    ScopedTreeUpdater sampUpdate(widgets.samplers);

    for(const D3D9Pipe::Sampler &samp : samplers)
    {
      uint32_t reg = vs ? samp.samplerIndex - D3D9VertexSampler0 : samp.samplerIndex;
      QString slot = QFormatStr("s%1").arg(reg);
      bool used = isSamplerUsed(sh.stage, samp.samplerIndex);

      addTextureRow(widgets.textures, slot, samp, used);

      if(!showNode(used, samp.resourceId != ResourceId()))
        continue;

      QString addressing = QFormatStr("%1, %2, %3")
                               .arg(ToQStr(samp.addressU, GraphicsAPI::D3D9))
                               .arg(ToQStr(samp.addressV, GraphicsAPI::D3D9))
                               .arg(ToQStr(samp.addressW, GraphicsAPI::D3D9));
      QString filter = QFormatStr("Min %1, Mag %2, Mip %3")
                           .arg(ToQStr(samp.minFilter))
                           .arg(ToQStr(samp.magFilter))
                           .arg(ToQStr(samp.mipFilter));

      RDTreeWidgetItem *node = new RDTreeWidgetItem(
          {slot, addressing, filter, samp.maxMipLevel, Formatter::Format(samp.mipLODBias),
           samp.maxAnisotropy, samp.srgbTexture ? tr("Yes") : tr("No"),
           colorString(samp.borderColor)});
      if(!used)
        node->setItalic(true);
      widgets.samplers->addTopLevelItem(node);
    }

    // the displacement map is part of vertex processing
    if(vs)
    {
      const D3D9Pipe::Sampler &dmap = m_Ctx.CurD3D9PipelineState()->displacementMapSampler;
      addTextureRow(widgets.textures, tr("DMAP"), dmap,
                    isSamplerUsed(ShaderStage::Vertex, dmap.samplerIndex));
    }
  }

  // constants
  {
    ScopedTreeUpdater update(widgets.constants);

    struct
    {
      QString name;
      ResourceId id;
      uint32_t count;
      uint32_t regSize;
      QString format;
    } sets[] = {
        {tr("Float (c#)"), sh.floatConstantsBuffer, sh.floatConstantCount, 16, lit("float4 c;")},
        {tr("Integer (i#)"), sh.intConstantsBuffer, sh.intConstantCount, 16, lit("int4 i;")},
        {tr("Boolean (b#)"), sh.boolConstantsBuffer, sh.boolConstantCount, 4, lit("uint b;")},
    };

    rdcarray<UsedDescriptor> blocks = m_Ctx.CurPipelineState().GetConstantBlocks(sh.stage, false);

    for(auto &set : sets)
    {
      // find the shader's constant block for this register set
      int32_t block = -1;
      QString blockName;
      for(const UsedDescriptor &used : blocks)
      {
        if(used.descriptor.resource == set.id && refl &&
           used.access.index < refl->constantBlocks.size())
        {
          block = used.access.index;
          blockName = QString(refl->constantBlocks[block].name);
        }
      }

      bool used = block >= 0 || (sh.resourceId != ResourceId() && refl == NULL);

      if(!showNode(used, true))
        continue;

      RDTreeWidgetItem *node = new RDTreeWidgetItem(
          {set.name, set.id, set.count, blockName.isEmpty() ? tr("-") : blockName, QString()});
      node->setTag(QVariant::fromValue(
          D3D9ConstantsTag(sh.stage, set.id, set.count * set.regSize, set.format, block)));
      if(!used)
        node->setItalic(true);
      widgets.constants->addTopLevelItem(node);
    }
  }
}

void D3D9PipelineStateViewer::addTargetRow(const QString &slot, const Descriptor &desc)
{
  bool filled = desc.resource != ResourceId();

  if(!showNode(filled, filled))
    return;

  TextureDescription *tex = m_Ctx.GetTexture(desc.resource);

  RDTreeWidgetItem *node = NULL;

  if(tex)
    node = new RDTreeWidgetItem({slot, desc.resource, ToQStr(tex->type), tex->width, tex->height,
                                 desc.firstMip, desc.firstSlice, tex->format.Name(), QString()});
  else
    node = new RDTreeWidgetItem({slot, resourceName(desc.resource), QString(), QString(), QString(),
                                 QString(), QString(), QString(), QString()});

  node->setTag(QVariant::fromValue(desc.resource));

  if(!filled)
  {
    node->setBackgroundColor(QColor(255, 70, 70));
    node->setForegroundColor(QColor(0, 0, 0));
  }

  m_Targets->addTopLevelItem(node);
}

void D3D9PipelineStateViewer::setState()
{
  if(!m_Ctx.IsCaptureLoaded() || m_Ctx.CurD3D9PipelineState() == NULL)
  {
    clearState();
    return;
  }

  const D3D9Pipe::State &state = *m_Ctx.CurD3D9PipelineState();
  const ActionDescription *action = m_Ctx.CurAction();

  //////////////////////////////////////////////////////////////////////////
  // input assembly

  {
    const D3D9Pipe::InputAssembly &ia = state.inputAssembly;

    if(ia.declarationId != ResourceId())
      m_Declaration->setText(resourceName(ia.declarationId));
    else if(!ia.vertexElements.isEmpty())
      m_Declaration->setText(tr("From FVF 0x%1").arg(ia.fvf, 0, 16));
    else
      m_Declaration->setText(tr("None"));

    m_Topology->setText(ToQStr(ia.topology));
    m_Common.setTopologyDiagram(m_TopologyDiagram, ia.topology);

    // the diagrams are drawn at their SVG size, which is much taller than this row. Scale them to
    // about two lines of text, keeping each one's aspect ratio.
    {
      const QPixmap *px = m_TopologyDiagram->pixmap();
      if(px && !px->isNull())
      {
        const qreal dpr = px->devicePixelRatio();
        const qreal pw = px->width() / dpr, ph = px->height() / dpr;
        const int h = qMin(int(ph), QFontMetrics(m_Topology->font()).lineSpacing() * 2);
        m_TopologyDiagram->setFixedSize(qMax(1, int(pw * h / ph)), h);
      }
    }

    rdcarray<VertexInputAttribute> inputs = m_Ctx.CurPipelineState().GetVertexInputs();

    {
      ScopedTreeUpdater update(m_Elements);

      for(int i = 0; i < ia.vertexElements.count(); i++)
      {
        const D3D9Pipe::VertexElement &el = ia.vertexElements[i];
        bool used = i < inputs.count() ? inputs[i].used : true;

        if(!showNode(used, true))
          continue;

        RDTreeWidgetItem *node =
            new RDTreeWidgetItem({i, el.semanticName, el.semanticIndex, el.declType,
                                  el.format.Name(), el.stream, el.byteOffset, el.method, QString()});
        if(!used)
          node->setItalic(true);
        m_Elements->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_Streams);

      // streams the declaration reads from
      QSet<uint32_t> usedStreams;
      for(int i = 0; i < ia.vertexElements.count(); i++)
        if(i >= inputs.count() || inputs[i].used)
          usedStreams.insert(ia.vertexElements[i].stream);

      for(int i = 0; i < ia.streams.count(); i++)
      {
        const D3D9Pipe::StreamSource &s = ia.streams[i];
        bool filled = s.resourceId != ResourceId();
        bool used = usedStreams.contains((uint32_t)i);

        if(!showNode(used, filled))
          continue;

        QString freq = QString::number(s.frequency);
        if(s.indexedData)
          freq = tr("Indexed data, %1 instances").arg(s.frequency);
        else if(s.instanceData)
          freq = tr("Instance data, step %1").arg(s.frequency);

        BufferDescription *buf = m_Ctx.GetBuffer(s.resourceId);

        RDTreeWidgetItem *node = new RDTreeWidgetItem(
            {i, filled ? QVariant(s.resourceId) : QVariant(tr("Empty")), s.byteStride, s.byteOffset,
             freq, buf ? QVariant((qulonglong)buf->length) : QVariant(QString()), QString()});
        node->setTag(QVariant::fromValue(D3D9BufferTag(s.resourceId, s.byteOffset, UINT64_MAX,
                                                       m_Common.GetVBufferFormatString(i))));

        if(!filled)
        {
          node->setBackgroundColor(QColor(255, 70, 70));
          node->setForegroundColor(QColor(0, 0, 0));
        }
        if(!used)
          node->setItalic(true);

        m_Streams->addTopLevelItem(node);
      }

      const D3D9Pipe::IndexBuffer &ib = ia.indexBuffer;
      bool ibUsed = action && (action->flags & ActionFlags::Indexed);
      if(showNode(ibUsed, ib.resourceId != ResourceId()))
      {
        BufferDescription *buf = m_Ctx.GetBuffer(ib.resourceId);

        QString format = ib.byteStride == 4 ? lit("uint") : lit("ushort");
        format += lit(" indices[%1]").arg(RENDERDOC_NumVerticesPerPrimitive(ia.topology));

        uint64_t offset = action ? uint64_t(action->indexOffset) * ib.byteStride : 0;

        RDTreeWidgetItem *node = new RDTreeWidgetItem(
            {tr("Index"),
             ib.resourceId != ResourceId() ? QVariant(ib.resourceId) : QVariant(tr("Empty")),
             ib.byteStride, 0, QString(),
             buf ? QVariant((qulonglong)buf->length) : QVariant(QString()), QString()});
        node->setTag(QVariant::fromValue(D3D9BufferTag(ib.resourceId, offset, UINT64_MAX, format)));

        if(ib.resourceId == ResourceId())
        {
          node->setBackgroundColor(QColor(255, 70, 70));
          node->setForegroundColor(QColor(0, 0, 0));
        }
        if(!ibUsed)
          node->setItalic(true);

        m_Streams->addTopLevelItem(node);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // shaders

  setShaderState(state.vertexShader, state.vertexSamplers, m_Shaders[0]);
  setShaderState(state.pixelShader, state.pixelSamplers, m_Shaders[1]);

  //////////////////////////////////////////////////////////////////////////
  // fixed function

  {
    const D3D9Pipe::FixedFunction &ff = state.fixedFunction;
    const bool ffVertex = state.vertexShader.resourceId == ResourceId();
    const bool ffPixel = state.pixelShader.resourceId == ResourceId();

    {
      ScopedTreeUpdater update(m_Transforms);
      for(const D3D9Pipe::Transform &t : ff.transforms)
      {
        RDTreeWidgetItem *node =
            new RDTreeWidgetItem({t.name, matrixRow(t.matrix, 0), matrixRow(t.matrix, 1),
                                  matrixRow(t.matrix, 2), matrixRow(t.matrix, 3)});
        if(!ffVertex)
          node->setItalic(true);
        m_Transforms->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_Lighting);
      QList<QPair<QString, QString>> rows = {
          {tr("Lighting"), ff.lighting ? tr("Enabled") : tr("Disabled")},
          {tr("Ambient"), colorString(ff.ambient)},
          {tr("Specular"), ff.specularEnable ? tr("Enabled") : tr("Disabled")},
          {tr("Normalize Normals"), ff.normalizeNormals ? tr("Yes") : tr("No")},
          {tr("Local Viewer"), ff.localViewer ? tr("Yes") : tr("No")},
          {tr("Color Vertex"), ff.colorVertex ? tr("Yes") : tr("No")},
          {tr("Diffuse Source"), ff.diffuseMaterialSource},
          {tr("Ambient Source"), ff.ambientMaterialSource},
          {tr("Specular Source"), ff.specularMaterialSource},
          {tr("Emissive Source"), ff.emissiveMaterialSource},
          {tr("Material Diffuse"), colorString(ff.material.diffuse)},
          {tr("Material Ambient"), colorString(ff.material.ambient)},
          {tr("Material Specular"), colorString(ff.material.specular)},
          {tr("Material Emissive"), colorString(ff.material.emissive)},
          {tr("Material Power"), Formatter::Format(ff.material.power)},
          {tr("Vertex Blend"), ff.vertexBlend},
          {tr("Indexed Vertex Blend"), ff.indexedVertexBlend ? tr("Yes") : tr("No")},
          {tr("Shade Mode"), ff.shadeMode},
      };
      for(const QPair<QString, QString> &r : rows)
      {
        RDTreeWidgetItem *node = new RDTreeWidgetItem({r.first, r.second});
        if(!ffVertex)
          node->setItalic(true);
        m_Lighting->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_Lights);
      for(const D3D9Pipe::Light &l : ff.lights)
      {
        if(!showNode(l.enabled, true))
          continue;

        RDTreeWidgetItem *node = new RDTreeWidgetItem(
            {l.index, l.enabled ? tr("Yes") : tr("No"), l.type, colorString(l.diffuse),
             colorString(l.specular), colorString(l.ambient), colorString(l.position),
             colorString(l.direction), Formatter::Format(l.range),
             QFormatStr("%1, %2, %3")
                 .arg(Formatter::Format(l.attenuation0))
                 .arg(Formatter::Format(l.attenuation1))
                 .arg(Formatter::Format(l.attenuation2)),
             tr("Theta %1, Phi %2, Falloff %3")
                 .arg(Formatter::Format(l.theta))
                 .arg(Formatter::Format(l.phi))
                 .arg(Formatter::Format(l.falloff))});
        if(!ffVertex || !l.enabled)
          node->setItalic(true);
        m_Lights->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_TextureStages);
      bool disabled = false;
      for(const D3D9Pipe::TextureStage &ts : ff.textureStages)
      {
        // stages after the first disabled one aren't used
        if(ts.colorOp == "DISABLE")
          disabled = true;

        bool used = ffPixel && !disabled;
        if(!showNode(used, ts.colorOp != "DISABLE"))
          continue;

        RDTreeWidgetItem *node = new RDTreeWidgetItem(
            {ts.stage, ts.colorOp, ts.colorArg1, ts.colorArg2, ts.colorArg0, ts.alphaOp,
             ts.alphaArg1, ts.alphaArg2, ts.alphaArg0, ts.resultArg,
             QFormatStr("%1 (%2)").arg(ts.texCoordIndex).arg(QString(ts.texCoordGeneration)),
             ts.textureTransformFlags, colorString(ts.constant)});
        if(!used)
          node->setItalic(true);
        m_TextureStages->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_Fog);
      QList<QPair<QString, QString>> rows = {
          {tr("Fog"), ff.fog.enabled ? tr("Enabled") : tr("Disabled")},
          {tr("Color"), colorString(ff.fog.color)},
          {tr("Pixel (Table) Mode"), ff.fog.tableMode},
          {tr("Vertex Mode"), ff.fog.vertexMode},
          {tr("Start"), Formatter::Format(ff.fog.start)},
          {tr("End"), Formatter::Format(ff.fog.end)},
          {tr("Density"), Formatter::Format(ff.fog.density)},
          {tr("Range Fog"), ff.fog.rangeFog ? tr("Yes") : tr("No")},
          {tr("Texture Factor"), colorString(ff.textureFactor)},
      };
      for(const QPair<QString, QString> &r : rows)
        m_Fog->addTopLevelItem(new RDTreeWidgetItem({r.first, r.second}));
    }

    {
      ScopedTreeUpdater update(m_ClipPlanes);
      for(int i = 0; i < ff.clipPlanes.count(); i++)
      {
        const D3D9Pipe::ClipPlane &p = ff.clipPlanes[i];
        if(!showNode(p.enabled, true))
          continue;
        RDTreeWidgetItem *node =
            new RDTreeWidgetItem({i, p.enabled ? tr("Yes") : tr("No"), colorString(p.plane)});
        if(!p.enabled)
          node->setItalic(true);
        m_ClipPlanes->addTopLevelItem(node);
      }
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // rasterizer

  {
    const D3D9Pipe::Rasterizer &rs = state.rasterizer;

    {
      ScopedTreeUpdater update(m_RasterState);
      QList<QPair<QString, QString>> rows = {
          {tr("Fill Mode"), ToQStr(rs.fillMode)},
          {tr("Cull Mode"), ToQStr(rs.cullMode) + tr(" (front faces are clockwise)")},
          {tr("Depth Bias"), Formatter::Format(rs.depthBias)},
          {tr("Slope Scaled Depth Bias"), Formatter::Format(rs.slopeScaledDepthBias)},
          {tr("Multisample Antialias"), rs.multisampleEnable ? tr("Yes") : tr("No")},
          {tr("Multisample Mask"), Formatter::Format(rs.multisampleMask, true)},
          {tr("Antialiased Lines"), rs.antialiasedLines ? tr("Yes") : tr("No")},
          {tr("Clipping"), rs.clipping ? tr("Yes") : tr("No")},
      };
      for(const QPair<QString, QString> &r : rows)
        m_RasterState->addTopLevelItem(new RDTreeWidgetItem({r.first, r.second}));
    }

    {
      ScopedTreeUpdater update(m_Viewport);
      m_Viewport->addTopLevelItem(new RDTreeWidgetItem(
          {tr("Viewport"), tr("Yes"), rs.viewport.x, rs.viewport.y, rs.viewport.width,
           rs.viewport.height, rs.viewport.minDepth, rs.viewport.maxDepth}));

      RDTreeWidgetItem *node = new RDTreeWidgetItem(
          {tr("Scissor"), rs.scissor.enabled ? tr("Yes") : tr("No"), rs.scissor.x, rs.scissor.y,
           rs.scissor.width, rs.scissor.height, QString(), QString()});
      if(!rs.scissor.enabled)
        node->setItalic(true);
      m_Viewport->addTopLevelItem(node);
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // output merger

  {
    const D3D9Pipe::OutputMerger &om = state.outputMerger;

    {
      ScopedTreeUpdater update(m_Targets);
      for(int i = 0; i < om.renderTargets.count(); i++)
        addTargetRow(QString::number(i), om.renderTargets[i]);
      addTargetRow(tr("Depth"), om.depthTarget);
    }

    {
      ScopedTreeUpdater update(m_Blends);
      for(int i = 0; i < om.blendState.blends.count(); i++)
      {
        const ColorBlend &b = om.blendState.blends[i];
        bool filled = i < om.renderTargets.count() && om.renderTargets[i].resource != ResourceId();
        if(!showNode(filled, filled))
          continue;

        QString mask = QFormatStr("%1%2%3%4")
                           .arg((b.writeMask & 0x1) ? lit("R") : lit("_"))
                           .arg((b.writeMask & 0x2) ? lit("G") : lit("_"))
                           .arg((b.writeMask & 0x4) ? lit("B") : lit("_"))
                           .arg((b.writeMask & 0x8) ? lit("A") : lit("_"));

        RDTreeWidgetItem *node = new RDTreeWidgetItem(
            {i, b.enabled ? tr("True") : tr("False"), ToQStr(b.colorBlend.source),
             ToQStr(b.colorBlend.destination), ToQStr(b.colorBlend.operation),
             ToQStr(b.alphaBlend.source), ToQStr(b.alphaBlend.destination),
             ToQStr(b.alphaBlend.operation), mask});
        if(!b.enabled)
          node->setItalic(true);
        m_Blends->addTopLevelItem(node);
      }
    }

    {
      ScopedTreeUpdater update(m_OutputState);
      const rdcfixedarray<float, 4> &f = om.blendState.blendFactor;
      QList<QPair<QString, QString>> rows = {
          {tr("Separate Alpha Blend"), om.blendState.separateAlphaBlend ? tr("Yes") : tr("No")},
          {tr("Blend Factor"), colorString(FloatVector(f[0], f[1], f[2], f[3]))},
          {tr("Alpha Test"), om.alphaTest.enabled ? tr("Enabled") : tr("Disabled")},
          {tr("Alpha Test Function"), ToQStr(om.alphaTest.function)},
          {tr("Alpha Reference"), QString::number(om.alphaTest.reference)},
          {tr("sRGB Write"), om.srgbWrite ? tr("Yes") : tr("No")},
          {tr("Dither"), om.dither ? tr("Yes") : tr("No")},
      };
      for(const QPair<QString, QString> &r : rows)
        m_OutputState->addTopLevelItem(new RDTreeWidgetItem({r.first, r.second}));
    }

    const D3D9Pipe::DepthStencilState &ds = om.depthStencilState;

    {
      ScopedTreeUpdater update(m_DepthState);
      QList<QPair<QString, QString>> rows = {
          {tr("Depth Test"), ds.depthEnable ? tr("Enabled") : tr("Disabled")},
          {tr("W-Buffer"), ds.wBuffer ? tr("Yes") : tr("No")},
          {tr("Depth Writes"), ds.depthWrites ? tr("Enabled") : tr("Disabled")},
          {tr("Depth Function"), ToQStr(ds.depthFunction)},
          {tr("Stencil Test"), ds.stencilEnable ? tr("Enabled") : tr("Disabled")},
          {tr("Two-Sided Stencil"), ds.twoSidedStencil ? tr("Enabled") : tr("Disabled")},
          {tr("Stencil Reference"), Formatter::Format(ds.frontFace.reference, true)},
          {tr("Stencil Mask"), Formatter::Format(ds.frontFace.compareMask, true)},
          {tr("Stencil Write Mask"), Formatter::Format(ds.frontFace.writeMask, true)},
      };
      for(const QPair<QString, QString> &r : rows)
        m_DepthState->addTopLevelItem(new RDTreeWidgetItem({r.first, r.second}));
    }

    {
      ScopedTreeUpdater update(m_Stencils);

      RDTreeWidgetItem *front = new RDTreeWidgetItem(
          {tr("Front (CW)"), ToQStr(ds.frontFace.function), ToQStr(ds.frontFace.failOperation),
           ToQStr(ds.frontFace.depthFailOperation), ToQStr(ds.frontFace.passOperation)});
      if(!ds.stencilEnable)
        front->setItalic(true);
      m_Stencils->addTopLevelItem(front);

      RDTreeWidgetItem *back = new RDTreeWidgetItem(
          {tr("Back (CCW)"), ToQStr(ds.backFace.function), ToQStr(ds.backFace.failOperation),
           ToQStr(ds.backFace.depthFailOperation), ToQStr(ds.backFace.passOperation)});
      if(!ds.stencilEnable || !ds.twoSidedStencil)
        back->setItalic(true);
      m_Stencils->addTopLevelItem(back);
    }
  }

  //////////////////////////////////////////////////////////////////////////
  // every render state

  {
    ScopedTreeUpdater update(m_RenderStates);
    for(const D3D9Pipe::RenderState &r : state.renderStates)
    {
      float f;
      memcpy(&f, &r.value, sizeof(f));
      m_RenderStates->addTopLevelItem(
          new RDTreeWidgetItem({r.name, Formatter::Format(r.value, true), Formatter::Format(f)}));
    }
  }
}

const D3D9Pipe::Shader *D3D9PipelineStateViewer::stageForSender(QWidget *widget)
{
  if(!m_Ctx.IsCaptureLoaded() || m_Ctx.CurD3D9PipelineState() == NULL)
    return NULL;

  const D3D9Pipe::Shader *stages[] = {&m_Ctx.CurD3D9PipelineState()->vertexShader,
                                      &m_Ctx.CurD3D9PipelineState()->pixelShader};

  for(int s = 0; s < 2; s++)
  {
    const ShaderWidgets &w = m_Shaders[s];
    if(widget == w.shader || widget == w.view || widget == w.save || widget == w.textures ||
       widget == w.samplers || widget == w.constants)
      return stages[s];
  }

  return NULL;
}

void D3D9PipelineStateViewer::shaderView_clicked()
{
  const D3D9Pipe::Shader *stage = stageForSender(qobject_cast<QWidget *>(QObject::sender()));

  if(stage == NULL || stage->resourceId == ResourceId() || stage->reflection == NULL)
    return;

  IShaderViewer *shad = m_Ctx.ViewShader(stage->reflection, ResourceId());

  m_Ctx.AddDockWindow(shad->Widget(), DockReference::AddTo, this);
}

void D3D9PipelineStateViewer::shaderSave_clicked()
{
  const D3D9Pipe::Shader *stage = stageForSender(qobject_cast<QWidget *>(QObject::sender()));

  if(stage == NULL || stage->resourceId == ResourceId() || stage->reflection == NULL)
    return;

  m_Common.SaveShaderFile(stage->reflection);
}

void D3D9PipelineStateViewer::resource_itemActivated(RDTreeWidgetItem *item, int column)
{
  if(item == NULL)
    return;

  QVariant tag = item->tag();

  if(tag.canConvert<D3D9ConstantsTag>())
  {
    D3D9ConstantsTag c = tag.value<D3D9ConstantsTag>();

    if(c.block >= 0)
    {
      // the shader's reflection describes this register set, show its variables
      IBufferViewer *prev = m_Ctx.ViewConstantBuffer(c.stage, (uint32_t)c.block, 0);
      m_Ctx.AddDockWindow(prev->Widget(), DockReference::TransientPopupArea, this, 0.3f);
    }
    else if(c.id != ResourceId())
    {
      // otherwise show the raw registers, one per row
      IBufferViewer *viewer = m_Ctx.ViewBuffer(0, c.size, c.id, c.format);
      m_Ctx.AddDockWindow(viewer->Widget(), DockReference::AddTo, this);
    }
    return;
  }

  if(tag.canConvert<D3D9BufferTag>())
  {
    D3D9BufferTag buf = tag.value<D3D9BufferTag>();

    if(buf.id != ResourceId())
    {
      IBufferViewer *viewer = m_Ctx.ViewBuffer(buf.offset, buf.size, buf.id, buf.format);
      m_Ctx.AddDockWindow(viewer->Widget(), DockReference::AddTo, this);
    }
    return;
  }

  if(tag.canConvert<ResourceId>())
  {
    ResourceId id = tag.value<ResourceId>();

    TextureDescription *tex = m_Ctx.GetTexture(id);
    BufferDescription *buf = m_Ctx.GetBuffer(id);

    if(tex)
    {
      if(!m_Ctx.HasTextureViewer())
        m_Ctx.ShowTextureViewer();
      ITextureViewer *viewer = m_Ctx.GetTextureViewer();
      viewer->ViewTexture(tex->resourceId, CompType::Typeless, true);
    }
    else if(buf)
    {
      IBufferViewer *viewer = m_Ctx.ViewBuffer(0, buf->length, id);
      m_Ctx.AddDockWindow(viewer->Widget(), DockReference::AddTo, this);
    }
  }
}

void D3D9PipelineStateViewer::on_pipeFlow_stageSelected(int index)
{
  ui->stagesTabs->setCurrentIndex(index);
}

void D3D9PipelineStateViewer::on_meshView_clicked()
{
  if(!m_Ctx.HasMeshPreview())
    m_Ctx.ShowMeshPreview();
  ToolWindowManager::raiseToolWindow(m_Ctx.GetMeshPreview()->Widget());
}

void D3D9PipelineStateViewer::exportHTML(QXmlStreamWriter &xml, Page page)
{
  if(page == Page_IA)
  {
    m_Common.exportHTMLTable(xml, {tr("Vertex Declaration"), tr("Topology")},
                             {m_Declaration->text(), m_Topology->text()});
    xml.writeStartElement(lit("p"));
    xml.writeEndElement();
  }
  else if(page == Page_VS || page == Page_PS)
  {
    const ShaderWidgets &w = m_Shaders[page == Page_VS ? 0 : 1];
    m_Common.exportHTMLTable(xml, {tr("Shader")}, {w.shader->text()});
    xml.writeStartElement(lit("p"));
    xml.writeEndElement();
  }

  // every table on the page, as it's currently shown
  for(const Table &t : m_Tables)
  {
    if(t.page != page)
      continue;

    xml.writeStartElement(lit("h3"));
    xml.writeCharacters(t.title);
    xml.writeEndElement();

    QStringList cols = t.columns;
    bool goColumn = !cols.isEmpty() && cols.last() == tr("Go");
    if(goColumn)
      cols.removeLast();

    QList<QVariantList> rows;
    for(int i = 0; i < t.tree->topLevelItemCount(); i++)
    {
      RDTreeWidgetItem *item = t.tree->topLevelItem(i);
      QVariantList row;
      for(int c = 0; c < cols.count(); c++)
        row.push_back(item->text(c));
      rows.push_back(row);
    }

    m_Common.exportHTMLTable(xml, cols, rows);

    xml.writeStartElement(lit("p"));
    xml.writeEndElement();
  }
}

void D3D9PipelineStateViewer::on_exportHTML_clicked()
{
  QXmlStreamWriter *xmlptr = m_Common.beginHTMLExport();

  if(xmlptr)
  {
    QXmlStreamWriter &xml = *xmlptr;

    const QStringList &stageNames = ui->pipeFlow->stageNames();
    const QStringList &stageAbbrevs = ui->pipeFlow->stageAbbreviations();

    for(int stage = 0; stage < stageNames.count() && stage < Page_Count; stage++)
    {
      xml.writeStartElement(lit("div"));
      xml.writeStartElement(lit("a"));
      xml.writeAttribute(lit("name"), stageAbbrevs[stage]);
      xml.writeEndElement();
      xml.writeEndElement();

      xml.writeStartElement(lit("div"));
      xml.writeAttribute(lit("class"), lit("stage"));

      xml.writeStartElement(lit("h1"));
      xml.writeCharacters(stageNames[stage]);
      xml.writeEndElement();

      exportHTML(xml, (Page)stage);

      xml.writeEndElement();
    }

    m_Common.endHTMLExport(xmlptr);
  }
}
