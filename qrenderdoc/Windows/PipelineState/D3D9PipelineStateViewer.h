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

#pragma once

#include <QFrame>
#include "Code/Interface/QRDInterface.h"

namespace Ui
{
class D3D9PipelineStateViewer;
}

class QLabel;
class QToolButton;
class QVBoxLayout;
class QXmlStreamWriter;

class RDLabel;
class RDTreeWidget;
class RDTreeWidgetItem;
class PipelineStateViewer;

// The D3D9 pipeline state. The tables on each page are built in code (see the constructor), the
// .ui file only has the toolbar, the pipeline flow chart and the empty stage pages.
class D3D9PipelineStateViewer : public QFrame, public ICaptureViewer
{
  Q_OBJECT

public:
  explicit D3D9PipelineStateViewer(ICaptureContext &ctx, PipelineStateViewer &common,
                                   QWidget *parent = 0);
  ~D3D9PipelineStateViewer();

  // ICaptureViewer
  void OnCaptureLoaded() override;
  void OnCaptureClosed() override;
  void OnSelectedEventChanged(uint32_t eventId) override {}
  void OnEventChanged(uint32_t eventId) override;

  void SelectPipelineStage(PipelineStage stage);
  ResourceId GetResource(RDTreeWidgetItem *item);

private slots:
  // automatic slots
  void on_showUnused_toggled(bool checked);
  void on_showEmpty_toggled(bool checked);
  void on_exportHTML_clicked();
  void on_meshView_clicked();
  void on_pipeFlow_stageSelected(int index);

  // manual slots
  void shaderView_clicked();
  void shaderSave_clicked();
  void resource_itemActivated(RDTreeWidgetItem *item, int column);

private:
  enum Page
  {
    Page_IA,
    Page_VS,
    Page_RS,
    Page_PS,
    Page_OM,
    Page_States,
    Page_Count,
  };

  struct Table
  {
    Page page;
    QString title;
    QStringList columns;
    RDTreeWidget *tree;
  };

  struct ShaderWidgets
  {
    RDLabel *shader = NULL;
    QToolButton *view = NULL;
    QToolButton *save = NULL;
    RDTreeWidget *textures = NULL;
    RDTreeWidget *samplers = NULL;
    RDTreeWidget *constants = NULL;
  };

  Ui::D3D9PipelineStateViewer *ui;
  ICaptureContext &m_Ctx;
  PipelineStateViewer &m_Common;

  QVBoxLayout *m_PageLayouts[Page_Count] = {};
  QList<Table> m_Tables;

  // input assembly
  RDLabel *m_Declaration = NULL;
  QLabel *m_Topology = NULL;
  QLabel *m_TopologyDiagram = NULL;
  RDTreeWidget *m_Elements = NULL;
  RDTreeWidget *m_Streams = NULL;

  // shaders, vertex then pixel
  ShaderWidgets m_Shaders[2];

  // fixed function
  RDTreeWidget *m_TextureStages = NULL;
  RDTreeWidget *m_Transforms = NULL;
  RDTreeWidget *m_Lights = NULL;
  RDTreeWidget *m_Lighting = NULL;
  RDTreeWidget *m_Fog = NULL;

  // rasterizer
  RDTreeWidget *m_RasterState = NULL;
  RDTreeWidget *m_Viewport = NULL;
  RDTreeWidget *m_ClipPlanes = NULL;

  // output merger
  RDTreeWidget *m_Targets = NULL;
  RDTreeWidget *m_Blends = NULL;
  RDTreeWidget *m_OutputState = NULL;
  RDTreeWidget *m_DepthState = NULL;
  RDTreeWidget *m_Stencils = NULL;

  // every render state
  RDTreeWidget *m_RenderStates = NULL;

  QVBoxLayout *makePage(Page page);
  RDTreeWidget *addTable(Page page, const QString &title, const QStringList &columns,
                         const QList<int> &stretch, bool goColumn);
  void setupShaderPage(Page page, ShaderWidgets &widgets);

  void clearState();
  void setState();
  void setShaderState(const D3D9Pipe::Shader &sh, const rdcarray<D3D9Pipe::Sampler> &samplers,
                      ShaderWidgets &widgets);
  void addTextureRow(RDTreeWidget *tree, const QString &slot, const D3D9Pipe::Sampler &samp,
                     bool used);
  void addTargetRow(const QString &slot, const Descriptor &desc);

  bool showNode(bool usedSlot, bool filledSlot);
  bool isSamplerUsed(ShaderStage stage, uint32_t samplerIndex);

  const D3D9Pipe::Shader *stageForSender(QWidget *widget);

  QString resourceName(ResourceId id);
  QString colorString(const FloatVector &c);
  QString matrixRow(const rdcfixedarray<float, 16> &m, int row);

  void exportHTML(QXmlStreamWriter &xml, Page page);
};
