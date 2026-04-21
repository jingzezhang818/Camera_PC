/********************************************************************************
** Form generated from reading UI file 'widget.ui'
**
** Created by: Qt User Interface Compiler version 5.12.10
**
** WARNING! All changes made in this file will be lost when recompiling UI file!
********************************************************************************/

#ifndef UI_WIDGET_H
#define UI_WIDGET_H

#include <QtCore/QVariant>
#include <QtWidgets/QApplication>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpacerItem>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

QT_BEGIN_NAMESPACE

class Ui_Widget
{
public:
    QVBoxLayout *verticalLayout;
    QHBoxLayout *horizontalLayout;
    QPushButton *btnListModes;
    QPushButton *btnGrabOneFrame;
    QSpacerItem *horizontalSpacer;
    QPlainTextEdit *plainTextEdit;

    void setupUi(QWidget *Widget)
    {
        if (Widget->objectName().isEmpty())
            Widget->setObjectName(QString::fromUtf8("Widget"));
        Widget->resize(900, 600);
        verticalLayout = new QVBoxLayout(Widget);
        verticalLayout->setObjectName(QString::fromUtf8("verticalLayout"));
        horizontalLayout = new QHBoxLayout();
        horizontalLayout->setObjectName(QString::fromUtf8("horizontalLayout"));
        btnListModes = new QPushButton(Widget);
        btnListModes->setObjectName(QString::fromUtf8("btnListModes"));

        horizontalLayout->addWidget(btnListModes);

        btnGrabOneFrame = new QPushButton(Widget);
        btnGrabOneFrame->setObjectName(QString::fromUtf8("btnGrabOneFrame"));

        horizontalLayout->addWidget(btnGrabOneFrame);

        horizontalSpacer = new QSpacerItem(40, 20, QSizePolicy::Expanding, QSizePolicy::Minimum);

        horizontalLayout->addItem(horizontalSpacer);


        verticalLayout->addLayout(horizontalLayout);

        plainTextEdit = new QPlainTextEdit(Widget);
        plainTextEdit->setObjectName(QString::fromUtf8("plainTextEdit"));
        plainTextEdit->setReadOnly(true);

        verticalLayout->addWidget(plainTextEdit);


        retranslateUi(Widget);

        QMetaObject::connectSlotsByName(Widget);
    } // setupUi

    void retranslateUi(QWidget *Widget)
    {
        Widget->setWindowTitle(QApplication::translate("Widget", "Camera Probe", nullptr));
        btnListModes->setText(QApplication::translate("Widget", "\345\210\227\345\207\272\346\250\241\345\274\217", nullptr));
        btnGrabOneFrame->setText(QApplication::translate("Widget", "\351\207\207\344\270\200\345\270\247", nullptr));
    } // retranslateUi

};

namespace Ui {
    class Widget: public Ui_Widget {};
} // namespace Ui

QT_END_NAMESPACE

#endif // UI_WIDGET_H
