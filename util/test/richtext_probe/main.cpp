// Measures how qrenderdoc's RichResourceText rows lay out with the Qt this is built against, to
// find a fix for Event Browser rows that are drawn low and clipped. It builds each row's HTML the
// way RichResourceText::cacheDocument does, paints it the way RichResourceTextPaint does into a
// row as tall as the Event Browser's (uniform rows sized from a plain text row), and measures
// where the text's ink ends up compared to a plain row. Several variants of the HTML are tried.
//
// usage: richtext_probe [font scale, default 1.0] [output dir for PNGs, default .]
// The font scale is qrenderdoc's Settings > General > Global font scale. Build on Windows with
// build.bat, against qrenderdoc's bundled Qt, so the result matches qrenderdoc's. It also builds
// on Linux against the system Qt (g++ main.cpp with the Qt5Widgets flags).

#include <stdio.h>
#include <QtWidgets>

struct Ink
{
  int top = -1, bottom = -1;
};

// ink rows of dark pixels in an image drawn black on white
static Ink FindInk(const QImage &im, int yOffset)
{
  Ink ret;
  for(int y = 0; y < im.height(); y++)
  {
    const QRgb *line = (const QRgb *)im.constScanLine(y);
    for(int x = 0; x < im.width(); x++)
    {
      if(qGray(line[x]) < 128)
      {
        if(ret.top < 0)
          ret.top = y - yOffset;
        ret.bottom = y - yOffset;
        break;
      }
    }
  }
  return ret;
}

int main(int argc, char **argv)
{
  // same as qrenderdoc
  QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
  QGuiApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if(QT_VERSION >= QT_VERSION_CHECK(5, 14, 0))
  QGuiApplication::setHighDpiScaleFactorRoundingPolicy(
      Qt::HighDpiScaleFactorRoundingPolicy::RoundPreferFloor);
#endif

  QApplication app(argc, argv);

  const double scale = argc > 1 ? atof(argv[1]) : 1.0;
  const QString outDir = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral(".");

  QFont font = QApplication::font();
  font.setPointSizeF(font.pointSizeF() * scale);

  QWidget probe;
  probe.setFont(font);

  QFontMetrics m(font);

  // a plain row's height in the Event Browser: QStyledItemDelegate's hint, at least the decoration
  // size or font height plus 2 (RDTreeViewDelegate with ignoreIconSize and no vertical margin)
  int rowH = 0;
  {
    QTreeView view;
    view.setFont(font);
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QStringLiteral("WaitForRenderJobs")));
    view.setModel(&model);
    QStyleOptionViewItem opt;
    opt.initFrom(&view);
    opt.font = font;
    opt.fontMetrics = m;
    opt.decorationSize = view.iconSize().isValid() ? view.iconSize() : QSize(16, 16);
    QStyledItemDelegate del;
    QSize hint = del.sizeHint(opt, model.index(0, 0));
    int minHeight = m.height();
    rowH = qMax(qMax(opt.decorationSize.height(), minHeight) + 2, hint.height());
    rowH = qMax(rowH, minHeight);
  }

  printf("Qt %s, font '%s' %.2fpt (scale %.2f), devicePixelRatio %.2f, logical DPI %d\n",
         qVersion(), font.family().toUtf8().data(), font.pointSizeF(), scale,
         probe.devicePixelRatioF(), probe.logicalDpiY());
  printf("font height %d, ascent %d, descent %d, lineSpacing %d; row height %d\n", m.height(),
         m.ascent(), m.descent(), m.lineSpacing(), rowH);
  printf("RichResourceTextHeightHint (1 line) = %d\n\n", m.lineSpacing() + 2 * 2);

  const int width = 900;

  // plain row, drawn like the item delegate draws text: vertically centred
  Ink plainInk;
  {
    QImage im(width, rowH * 3, QImage::Format_ARGB32);
    im.fill(Qt::white);
    QPainter p(&im);
    p.setFont(font);
    p.setPen(Qt::black);
    p.drawText(QRect(4, rowH, width - 8, rowH), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("CopySubresourceRegion(Buffer-16-16)"));
    p.end();
    plainInk = FindInk(im, rowH);
  }
  printf("plain row text ink: rows %d..%d of 0..%d\n\n", plainInk.top, plainInk.bottom, rowH - 1);

  const QString imgPath =
      QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("link.png"));
  const bool haveImg = QFile::exists(imgPath);
  if(!haveImg)
    printf("(no link.png next to the exe, the icon cell will be empty)\n\n");

  struct Variant
  {
    const char *name;
    QString style;
  };
  const QString ls = QString::number(m.lineSpacing());
  QVector<Variant> variants = {
      {"current: line-height 14px", QStringLiteral("line-height: 14px")},
      {"no line-height", QString()},
      {"line-height = font lineSpacing px", QStringLiteral("line-height: %1px").arg(ls)},
      {"line-height 14px, -qt-line-height-type minimum",
       QStringLiteral("line-height: 14px; -qt-line-height-type: minimum")},
      {"line-height 100%", QStringLiteral("line-height: 100%")},
      // inline text and image, no table
      {"no table, image vertical-align middle", QStringLiteral("notable")},
      // only Qt 5.14+ understands this: what the current HTML does if Qt treats px as fixed
      {"(5.14+ only) 14px, forced fixed",
       QStringLiteral("line-height: 14px; -qt-line-height-type: fixed")},
  };

  struct Row
  {
    const char *name;
    bool image;
  };
  const Row rows[] = {{"link", true}, {"params, no link", false}};

  for(int v = 0; v < variants.size(); v++)
  {
    for(const Row &row : rows)
    {
      const QString td =
          variants[v].style.isEmpty() || variants[v].style == QStringLiteral("notable")
              ? QStringLiteral("<td valign=\"middle\">")
              : QStringLiteral("<td valign=\"middle\" style=\"%1\">").arg(variants[v].style);

      QString html = QStringLiteral("<table><tr>");
      if(variants[v].style == QStringLiteral("notable"))
      {
        if(row.image)
          html = QStringLiteral(
                     "CopySubresourceRegion(<b>AsyncReadbackData-Staging</b>"
                     "<img width=\"16\" style=\"vertical-align: middle\" src='%1'>)")
                     .arg(imgPath);
        else
          html = QStringLiteral("DrawIndexed(<span style='color: #ff0000'>IndexCount</span>=6)");
      }
      else if(row.image)
      {
        html += td + QStringLiteral("CopySubresourceRegion(</td>");
        html += td + QStringLiteral("<b>AsyncReadbackData-Staging</b></td>");
        html += td + QStringLiteral("<img width=\"16\" src='%1'></td>").arg(imgPath);
        html += td + QStringLiteral(")</td>");
      }
      else
      {
        html += td + QStringLiteral(
                         "DrawIndexed(<span style='color: #ff0000'>IndexCount</span>=6)</td>");
      }
      if(variants[v].style != QStringLiteral("notable"))
        html += QStringLiteral("</tr></table>");

      QTextDocument doc;
      doc.setDocumentMargin(0);
      doc.setHtml(html);
      doc.setDefaultFont(font);
      doc.setTextWidth(-1);
      doc.setTextWidth(10000);

      const int docH = qRound(doc.size().height());

      // paint like RichResourceTextPaint, but without the clip and into a taller image so we can
      // see how far the text overflows the row
      QImage im(width, rowH * 3, QImage::Format_ARGB32);
      im.fill(Qt::white);
      {
        QPainter p(&im);
        p.translate(0, rowH);
        int diff = rowH - docH;
        if(diff > 0)
          p.translate(1, diff / 2);
        else
          p.translate(1, 0);
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, Qt::black);
        doc.documentLayout()->draw(&p, ctx);
      }

      // ink includes the link icon's dark pixels, which also show up clipped
      Ink ink = FindInk(im, rowH);

      const bool clipped = ink.top < 0 || ink.bottom >= rowH;
      const int offset = (ink.top + ink.bottom) / 2 - (plainInk.top + plainInk.bottom) / 2;

      printf("%-48s %-16s doc height %3d  ink %3d..%3d  %s, %+d px from plain row\n",
             variants[v].name, row.name, docH, ink.top, ink.bottom,
             clipped ? "CLIPPED" : (qAbs(offset) > 2 ? "SHIFTED" : "OK     "), offset);

      // save the row as it would look, with the clip and the plain row above for comparison
      QImage shown = im.copy(0, rowH, width, rowH);
      QImage pair(width, rowH * 2, QImage::Format_ARGB32);
      pair.fill(Qt::white);
      {
        QPainter p(&pair);
        p.setFont(font);
        p.setPen(Qt::black);
        p.drawText(QRect(4, 0, width - 8, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("CopySubresourceRegion(Buffer-16-16)  <- plain row"));
        p.drawImage(0, rowH, shown);
        p.setPen(Qt::gray);
        p.drawLine(0, rowH, width, rowH);
      }
      pair.save(QDir(outDir).filePath(
          QStringLiteral("variant%1_%2.png")
              .arg(v)
              .arg(row.image ? QStringLiteral("link") : QStringLiteral("params"))));
    }
  }

  printf(
      "\nA variant works if both its rows say OK: the text fits in the row and is within 2px\n"
      "of where a plain row's text is. The PNGs show each row under a plain one.\n");
  return 0;
}
