#include "mainpicker.h"
#include "./ui_mainpicker.h"
#include <QDebug>
#include <QtWidgets>

MainPicker::MainPicker(QWidget* parent) : QMainWindow(parent), ui(new Ui::MainPicker) {
    ui->setupUi(this);

    QCheckBox* cursorParam = new QCheckBox("Include Cursor", this);
    cursorParam->setObjectName("cursorCheck");
    cursorParam->setChecked(true);
    ui->verticalLayout->addWidget(cursorParam);

    QCheckBox* rotationParam = new QCheckBox("Enable Rotation Fix", this);
    rotationParam->setObjectName("rotationCheck");
    rotationParam->setChecked(true);
    ui->verticalLayout->addWidget(rotationParam);
}

MainPicker::~MainPicker() {
    delete ui;
}

void MainPicker::onMonitorButtonClicked(QObject* target, QEvent* event) {
    qDebug() << "click";
}
