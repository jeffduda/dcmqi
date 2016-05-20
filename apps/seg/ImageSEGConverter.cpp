#include "ImageSEGConverter.h"
#include <iostream>


namespace dcmqi {

    bool ImageSEGConverter::itkimage2dcmSegmentation(vector<string> dicomImageFileNames, vector<string> segmentationFileNames,
                                             const char *metaDataFileName, const char *outputFileName) {

        typedef short PixelType;
        typedef itk::Image<PixelType, 3> ImageType;
        typedef itk::ImageFileReader<ImageType> ReaderType;
        typedef itk::LabelImageToLabelMapFilter<ImageType> LabelToLabelMapFilterType;


        ReaderType::Pointer reader = ReaderType::New();
        reader->SetFileName(segmentationFileNames[0].c_str());
        reader->Update();
        ImageType::Pointer labelImage = reader->GetOutput();

        ImageType::SizeType inputSize = labelImage->GetBufferedRegion().GetSize();
        cout << "Input image size: " << inputSize << endl;

        JSONMetaInformationHandler metaInfo(metaDataFileName);

        IODGeneralEquipmentModule::EquipmentInfo eq = getEquipmentInfo();
        ContentIdentificationMacro ident = createContentIdentificationInformation();
        CHECK_COND(ident.setInstanceNumber(metaInfo.instanceNumber.c_str()));


        /* Create new segmentation document */
        DcmDataset segdocDataset;
        DcmSegmentation *segdoc = NULL;

        CHECK_COND(DcmSegmentation::createBinarySegmentation(
                segdoc,   // resulting segmentation
                inputSize[1],      // rows
                inputSize[0],      // columns
                eq,       // equipment
                ident));   // content identification

        /* Import patient and study from existing file */
        CHECK_COND(segdoc->importPatientStudyFoR(dicomImageFileNames[0].c_str(), OFTrue, OFTrue, OFFalse, OFTrue));

        /* Initialize dimension module */
        char dimUID[128];
        dcmGenerateUniqueIdentifier(dimUID, QIICR_UID_ROOT);
        IODMultiframeDimensionModule &mfdim = segdoc->getDimensions();
        CHECK_COND(mfdim.addDimensionIndex(DCM_ReferencedSegmentNumber, dimUID, DCM_SegmentIdentificationSequence,
                                           DcmTag(DCM_ReferencedSegmentNumber).getTagName()));
        CHECK_COND(mfdim.addDimensionIndex(DCM_ImagePositionPatient, dimUID, DCM_PlanePositionSequence,
                                           DcmTag(DCM_ImagePositionPatient).getTagName()));

        /* Initialize shared functional groups */
        FGInterface &segFGInt = segdoc->getFunctionalGroups();

        // Find mapping from the segmentation slice number to the derivation image
        // Assume that orientation of the segmentation is the same as the source series
        vector<int> slice2derimg(dicomImageFileNames.size());
        for(int i=0;i<dicomImageFileNames.size();i++){
            OFString ippStr;
            DcmFileFormat sliceFF;
            DcmDataset *sliceDataset = NULL;
            ImageType::PointType ippPoint;
            ImageType::IndexType ippIndex;
            CHECK_COND(sliceFF.loadFile(dicomImageFileNames[i].c_str()));
            sliceDataset = sliceFF.getDataset();
            for(int j=0;j<3;j++){
                CHECK_COND(sliceDataset->findAndGetOFString(DCM_ImagePositionPatient, ippStr, j));
                ippPoint[j] = atof(ippStr.c_str());
            }
            if(!labelImage->TransformPhysicalPointToIndex(ippPoint, ippIndex)){
                cerr << "ImagePositionPatient maps outside the ITK image!" << endl;
                cout << "image position: " << ippPoint << endl;
                cerr << "ippIndex: " << ippIndex << endl;
                return -1;
            }
            slice2derimg[ippIndex[2]] = i;
        }

        const unsigned frameSize = inputSize[0] * inputSize[1];


        // Shared FGs: PlaneOrientationPatientSequence
        {
            OFString imageOrientationPatientStr;

            ImageType::DirectionType labelDirMatrix = labelImage->GetDirection();

            cout << "Directions: " << labelDirMatrix << endl;

            FGPlaneOrientationPatient *planor =
                    FGPlaneOrientationPatient::createMinimal(
                            Helper::floatToStrScientific(labelDirMatrix[0][0]).c_str(),
                            Helper::floatToStrScientific(labelDirMatrix[1][0]).c_str(),
                            Helper::floatToStrScientific(labelDirMatrix[2][0]).c_str(),
                            Helper::floatToStrScientific(labelDirMatrix[0][1]).c_str(),
                            Helper::floatToStrScientific(labelDirMatrix[1][1]).c_str(),
                            Helper::floatToStrScientific(labelDirMatrix[2][1]).c_str());


            //CHECK_COND(planor->setImageOrientationPatient(imageOrientationPatientStr));
            CHECK_COND(segdoc->addForAllFrames(*planor));
        }

        // Shared FGs: PixelMeasuresSequence
        {
            FGPixelMeasures *pixmsr = new FGPixelMeasures();

            ImageType::SpacingType labelSpacing = labelImage->GetSpacing();
            ostringstream spacingSStream;
            spacingSStream << scientific << labelSpacing[0] << "\\" << labelSpacing[1];
            CHECK_COND(pixmsr->setPixelSpacing(spacingSStream.str().c_str()));

            spacingSStream.clear(); spacingSStream.str("");
            spacingSStream << scientific << labelSpacing[2];
            CHECK_COND(pixmsr->setSpacingBetweenSlices(spacingSStream.str().c_str()));
            CHECK_COND(pixmsr->setSliceThickness(spacingSStream.str().c_str()));
            CHECK_COND(segdoc->addForAllFrames(*pixmsr));
        }

        FGPlanePosPatient* fgppp = FGPlanePosPatient::createMinimal("1","1","1");
        FGFrameContent* fgfc = new FGFrameContent();
        FGDerivationImage* fgder = new FGDerivationImage();
        OFVector<FGBase*> perFrameFGs;

        perFrameFGs.push_back(fgppp);
        perFrameFGs.push_back(fgfc);
        perFrameFGs.push_back(fgder);

        // Iterate over the files and labels available in each file, create a segment for each label,
        //  initialize segment frames and add to the document

        OFString seriesInstanceUID, classUID;
        set<OFString> instanceUIDs;

        IODCommonInstanceReferenceModule &commref = segdoc->getCommonInstanceReference();
        OFVector<IODSeriesAndInstanceReferenceMacro::ReferencedSeriesItem*> &refseries = commref.getReferencedSeriesItems();

        IODSeriesAndInstanceReferenceMacro::ReferencedSeriesItem refseriesItem;
        refseries.push_back(&refseriesItem);

        OFVector<SOPInstanceReferenceMacro*> &refinstances = refseriesItem.getReferencedInstanceItems();

        DcmFileFormat ff;
        CHECK_COND(ff.loadFile(dicomImageFileNames[slice2derimg[0]].c_str()));
        DcmDataset *dcm = ff.getDataset();
        CHECK_COND(dcm->findAndGetOFString(DCM_SeriesInstanceUID, seriesInstanceUID));
        CHECK_COND(refseriesItem.setSeriesInstanceUID(seriesInstanceUID));

        int uidfound = 0, uidnotfound = 0;

        Uint8 *frameData = new Uint8[frameSize];
        for(int segFileNumber=0; segFileNumber<segmentationFileNames.size(); segFileNumber++){

            cout << "Processing input label " << segmentationFileNames[segFileNumber] << endl;

            LabelToLabelMapFilterType::Pointer l2lm = LabelToLabelMapFilterType::New();
            reader->SetFileName(segmentationFileNames[segFileNumber]);
            reader->Update();
            ImageType::Pointer labelImage = reader->GetOutput();

            l2lm->SetInput(labelImage);
            l2lm->Update();

            typedef LabelToLabelMapFilterType::OutputImageType::LabelObjectType LabelType;
            typedef itk::LabelStatisticsImageFilter<ImageType,ImageType> LabelStatisticsType;

            LabelStatisticsType::Pointer labelStats = LabelStatisticsType::New();

            cout << "Found " << l2lm->GetOutput()->GetNumberOfLabelObjects() << " label(s)" << endl;
            labelStats->SetInput(reader->GetOutput());
            labelStats->SetLabelInput(reader->GetOutput());
            labelStats->Update();

            bool cropSegmentsBBox = false;
            if(cropSegmentsBBox){
                cout << "WARNING: Crop operation enabled - WIP" << endl;
                typedef itk::BinaryThresholdImageFilter<ImageType,ImageType> ThresholdType;
                ThresholdType::Pointer thresh = ThresholdType::New();
                thresh->SetInput(reader->GetOutput());
                thresh->SetLowerThreshold(1);
                thresh->SetLowerThreshold(100);
                thresh->SetInsideValue(1);
                thresh->Update();

                LabelStatisticsType::Pointer threshLabelStats = LabelStatisticsType::New();

                threshLabelStats->SetInput(thresh->GetOutput());
                threshLabelStats->SetLabelInput(thresh->GetOutput());
                threshLabelStats->Update();

                LabelStatisticsType::BoundingBoxType threshBbox = threshLabelStats->GetBoundingBox(1);
                /*
                cout << "OVerall bounding box: " << threshBbox[0] << ", " << threshBbox[1]
                             << threshBbox[2] << ", " << threshBbox[3]
                             << threshBbox[4] << ", " << threshBbox[5]
                             << endl;
                             */
                return -1;//abort();
            }

            for(int segLabelNumber=0 ; segLabelNumber<l2lm->GetOutput()->GetNumberOfLabelObjects();segLabelNumber++){
                LabelType* labelObject = l2lm->GetOutput()->GetNthLabelObject(segLabelNumber);
                short label = labelObject->GetLabel();

                if(!label){
                    cout << "Skipping label 0" << endl;
                    continue;
                }

                cout << "Processing label " << label << endl;

                LabelStatisticsType::BoundingBoxType bbox = labelStats->GetBoundingBox(label);
                unsigned firstSlice, lastSlice;
                bool skipEmptySlices = true; // TODO: what to do with that line?
                if(skipEmptySlices){
                    firstSlice = bbox[4];
                    lastSlice = bbox[5]+1;
                } else {
                    firstSlice = 0;
                    lastSlice = inputSize[2];
                }

                cout << "Total non-empty slices that will be encoded in SEG for label " <<
                label << " is " << lastSlice-firstSlice << endl <<
                " (inclusive from " << firstSlice << " to " <<
                lastSlice << ")" << endl;

                DcmSegment* segment = NULL;
                string segFileName = segmentationFileNames[segFileNumber];

                SegmentAttributes* segmentAttributes = metaInfo.segmentsAttributes[segFileNumber];

                DcmSegTypes::E_SegmentAlgoType algoType;
                string algoName = "";
                string algoTypeStr = segmentAttributes->getSegmentAlgorithmType();
                if(algoTypeStr == "MANUAL"){
                    algoType = DcmSegTypes::SAT_MANUAL;
                } else {
                    if(algoTypeStr == "AUTOMATIC")
                        algoType = DcmSegTypes::SAT_AUTOMATIC;
                    if(algoTypeStr == "SEMIAUTOMATIC")
                        algoType = DcmSegTypes::SAT_SEMIAUTOMATIC;

                    algoName = segmentAttributes->getSegmentAlgorithmName();
                    if(algoName == ""){
                        cerr << "ERROR: Algorithm name must be specified for non-manual algorithm types!" << endl;
                        return -1;
                    }
                }

                CodeSequenceMacro* typeCode = segmentAttributes->getSegmentedPropertyType();
                CodeSequenceMacro* categoryCode = segmentAttributes->getSegmentedPropertyCategoryCode();
                assert(typeCode != NULL && categoryCode!= NULL);
                OFString segmentLabel;
                CHECK_COND(typeCode->getCodeMeaning(segmentLabel));
                CHECK_COND(DcmSegment::create(segment, segmentLabel, *categoryCode, *typeCode, algoType, algoName.c_str()));

                if(segmentAttributes->getSegmentDescription().length() > 0)
                    segment->setSegmentDescription(segmentAttributes->getSegmentDescription().c_str());

                CodeSequenceMacro* typeModifierCode = segmentAttributes->getSegmentedPropertyTypeModifier();
                if (typeModifierCode != NULL) {
                    OFVector<CodeSequenceMacro*>& modifiersVector = segment->getSegmentedPropertyTypeModifierCode();
                    modifiersVector.push_back(typeModifierCode);
                }

                GeneralAnatomyMacro &anatomyMacro = segment->getGeneralAnatomyCode();
                if (segmentAttributes->getAnatomicRegion() != NULL){
                    OFVector<CodeSequenceMacro*>& anatomyMacroModifiersVector = anatomyMacro.getAnatomicRegionModifier();
                    CodeSequenceMacro& anatomicRegion = anatomyMacro.getAnatomicRegion();
                    anatomicRegion = *segmentAttributes->getAnatomicRegion();

                    if(segmentAttributes->getAnatomicRegionModifier() != NULL){
                        CodeSequenceMacro* anatomicRegionModifier = segmentAttributes->getAnatomicRegionModifier();
                        anatomyMacroModifiersVector.push_back(anatomicRegionModifier);
                    }
                }

                // TODO: Maybe implement for PrimaryAnatomicStructure and PrimaryAnatomicStructureModifier

                unsigned* rgb = segmentAttributes->getRecommendedDisplayRGBValue();
                unsigned cielabScaled[3];
                float cielab[3], ciexyz[3];

                Helper::getCIEXYZFromRGB(&rgb[0],&ciexyz[0]);
                Helper::getCIELabFromCIEXYZ(&ciexyz[0],&cielab[0]);
                Helper::getIntegerScaledCIELabFromCIELab(&cielab[0],&cielabScaled[0]);
                CHECK_COND(segment->setRecommendedDisplayCIELabValue(cielabScaled[0],cielabScaled[1],cielabScaled[2]));

                Uint16 segmentNumber;
                CHECK_COND(segdoc->addSegment(segment, segmentNumber /* returns logical segment number */));

                // TODO: make it possible to skip empty frames (optional)
                // iterate over slices for an individual label and populate output frames
                for(int sliceNumber=firstSlice;sliceNumber<lastSlice;sliceNumber++){

                    // segments are numbered starting from 1
                    Uint32 frameNumber = (segmentNumber-1)*inputSize[2]+sliceNumber;

                    OFString imagePositionPatientStr;

                    // PerFrame FG: FrameContentSequence
                    //fracon->setStackID("1"); // all frames go into the same stack
                    CHECK_COND(fgfc->setDimensionIndexValues(segmentNumber, 0));
                    CHECK_COND(fgfc->setDimensionIndexValues(sliceNumber-firstSlice+1, 1));
                    //ostringstream inStackPosSStream; // StackID is not present/needed
                    //inStackPosSStream << s+1;
                    //fracon->setInStackPositionNumber(s+1);

                    // PerFrame FG: PlanePositionSequence
                    {
                        ImageType::PointType sliceOriginPoint;
                        ImageType::IndexType sliceOriginIndex;
                        sliceOriginIndex.Fill(0);
                        sliceOriginIndex[2] = sliceNumber;
                        labelImage->TransformIndexToPhysicalPoint(sliceOriginIndex, sliceOriginPoint);
                        ostringstream pppSStream;
                        if(sliceNumber>0){
                            ImageType::PointType prevOrigin;
                            ImageType::IndexType prevIndex;
                            prevIndex.Fill(0);
                            prevIndex[2] = sliceNumber-1;
                            labelImage->TransformIndexToPhysicalPoint(prevIndex, prevOrigin);
                        }
                        fgppp->setImagePositionPatient(
                                Helper::floatToStrScientific(sliceOriginPoint[0]).c_str(),
                                Helper::floatToStrScientific(sliceOriginPoint[1]).c_str(),
                                Helper::floatToStrScientific(sliceOriginPoint[2]).c_str());
                    }

                    /* Add frame that references this segment */
                    {
                        ImageType::RegionType sliceRegion;
                        ImageType::IndexType sliceIndex;
                        ImageType::SizeType sliceSize;

                        sliceIndex[0] = 0;
                        sliceIndex[1] = 0;
                        sliceIndex[2] = sliceNumber;

                        sliceSize[0] = inputSize[0];
                        sliceSize[1] = inputSize[1];
                        sliceSize[2] = 1;

                        sliceRegion.SetIndex(sliceIndex);
                        sliceRegion.SetSize(sliceSize);

                        unsigned framePixelCnt = 0;
                        itk::ImageRegionConstIteratorWithIndex<ImageType> sliceIterator(labelImage, sliceRegion);
                        for(sliceIterator.GoToBegin();!sliceIterator.IsAtEnd();++sliceIterator,++framePixelCnt){
                            if(sliceIterator.Get() == label){
                                frameData[framePixelCnt] = 1;
                                ImageType::IndexType idx = sliceIterator.GetIndex();
                                //cout << framePixelCnt << " " << idx[1] << "," << idx[0] << endl;
                            } else
                                frameData[framePixelCnt] = 0;
                        }

                        if(sliceNumber!=firstSlice)
                            Helper::checkValidityOfFirstSrcImage(segdoc);

                        FGDerivationImage* fgder = new FGDerivationImage();
                        perFrameFGs[2] = fgder;
                        //fgder->clearData();

                        if(sliceNumber!=firstSlice)
                            Helper::checkValidityOfFirstSrcImage(segdoc);

                        DerivationImageItem *derimgItem;
                        CHECK_COND(fgder->addDerivationImageItem(CodeSequenceMacro("113076","DCM","Segmentation"),"",derimgItem));

                        OFVector<OFString> siVector;

                        if(sliceNumber>=dicomImageFileNames.size()){
                            cerr << "ERROR: trying to access missing DICOM Slice! And sorry, multi-frame not supported at the moment..." << endl;
                            return -1;
                        }

                        siVector.push_back(OFString(dicomImageFileNames[slice2derimg[sliceNumber]].c_str()));

                        OFVector<SourceImageItem*> srcimgItems;
                        CHECK_COND(derimgItem->addSourceImageItems(siVector,
                                                                   CodeSequenceMacro("121322","DCM","Source image for image processing operation"),
                                                                   srcimgItems));

                        CHECK_COND(segdoc->addFrame(frameData, segmentNumber, perFrameFGs));

                        // check if frame 0 still has what we expect
                        Helper::checkValidityOfFirstSrcImage(segdoc);

                        if(1){
                            // initialize class UID and series instance UID
                            ImageSOPInstanceReferenceMacro &instRef = srcimgItems[0]->getImageSOPInstanceReference();
                            OFString instanceUID;
                            CHECK_COND(instRef.getReferencedSOPClassUID(classUID));
                            CHECK_COND(instRef.getReferencedSOPInstanceUID(instanceUID));

                            if(instanceUIDs.find(instanceUID) == instanceUIDs.end()){
                                SOPInstanceReferenceMacro *refinstancesItem = new SOPInstanceReferenceMacro();
                                CHECK_COND(refinstancesItem->setReferencedSOPClassUID(classUID));
                                CHECK_COND(refinstancesItem->setReferencedSOPInstanceUID(instanceUID));
                                refinstances.push_back(refinstancesItem);
                                instanceUIDs.insert(instanceUID);
                                uidnotfound++;
                            } else {
                                uidfound++;
                            }
                        }
                    }
                }
            }
        }

    //cout << "found:" << uidfound << " not: " << uidnotfound << endl;

    COUT << "Successfully created segmentation document" << OFendl;

    /* Store to disk */
    COUT << "Saving the result to " << outputFileName << OFendl;
    //segdoc->saveFile(outputFileName.c_str(), EXS_LittleEndianExplicit);

    CHECK_COND(segdoc->writeDataset(segdocDataset));

    // Set reader/session/timepoint information
    CHECK_COND(segdocDataset.putAndInsertString(DCM_ContentCreatorName, metaInfo.readerID.c_str()));
    CHECK_COND(segdocDataset.putAndInsertString(DCM_ClinicalTrialSeriesID, metaInfo.sessionID.c_str()));
    CHECK_COND(segdocDataset.putAndInsertString(DCM_ClinicalTrialTimePointID, metaInfo.timePointID.c_str()));
    CHECK_COND(segdocDataset.putAndInsertString(DCM_ClinicalTrialCoordinatingCenterName, "UIowa"));

    // populate BodyPartExamined
    {
        DcmFileFormat sliceFF;
        DcmDataset *sliceDataset = NULL;
        OFString bodyPartStr;
        string bodyPartAssigned = metaInfo.bodyPartExamined;

        CHECK_COND(sliceFF.loadFile(dicomImageFileNames[0].c_str()));

        sliceDataset = sliceFF.getDataset();

        // inherit BodyPartExamined from the source image dataset, if available
        if(sliceDataset->findAndGetOFString(DCM_BodyPartExamined, bodyPartStr).good())
        if(string(bodyPartStr.c_str()).size())
            bodyPartAssigned = bodyPartStr.c_str();

        if(bodyPartAssigned.size())
            CHECK_COND(segdocDataset.putAndInsertString(DCM_BodyPartExamined, bodyPartAssigned.c_str()));
    }

    // StudyDate/Time should be of the series segmented, not when segmentation was made - this is initialized by DCMTK

    // SeriesDate/Time should be of when segmentation was done; initialize to when it was saved
    {
        OFString contentDate, contentTime;
        DcmDate::getCurrentDate(contentDate);
        DcmTime::getCurrentTime(contentTime);

        segdocDataset.putAndInsertString(DCM_ContentDate, contentDate.c_str());
        segdocDataset.putAndInsertString(DCM_ContentTime, contentTime.c_str());
        segdocDataset.putAndInsertString(DCM_SeriesDate, contentDate.c_str());
        segdocDataset.putAndInsertString(DCM_SeriesTime, contentTime.c_str());

        segdocDataset.putAndInsertString(DCM_SeriesDescription, metaInfo.seriesDescription.c_str());
        segdocDataset.putAndInsertString(DCM_SeriesNumber, metaInfo.seriesNumber.c_str());
    }

    DcmFileFormat segdocFF(&segdocDataset);
    bool compress = false; // TODO: remove hardcoded
    if(compress){
        CHECK_COND(segdocFF.saveFile(outputFileName, EXS_DeflatedLittleEndianExplicit));
    } else {
        CHECK_COND(segdocFF.saveFile(outputFileName, EXS_LittleEndianExplicit));
    }

    COUT << "Saved segmentation as " << outputFileName << endl;
        return true;
    }

    bool ImageSEGConverter::dcmSegmentation2itkimage() {
        return true;
    }

    IODGeneralEquipmentModule::EquipmentInfo ImageSEGConverter::getEquipmentInfo() {
        IODGeneralEquipmentModule::EquipmentInfo eq;
        eq.m_Manufacturer = "QIICR";
        eq.m_DeviceSerialNumber = "0";
        eq.m_ManufacturerModelName = dcmqi_WC_URL;
        eq.m_SoftwareVersions = dcmqi_WC_REVISION;
        return eq;
    }

    ContentIdentificationMacro ImageSEGConverter::createContentIdentificationInformation() {
        ContentIdentificationMacro ident;
        CHECK_COND(ident.setContentCreatorName("QIICR"));
        CHECK_COND(ident.setContentDescription("Iowa QIN segmentation result"));
        CHECK_COND(ident.setContentLabel("QIICR QIN IOWA"));
        return ident;
    }

    int ImageSEGConverter::CHECK_COND(const OFCondition& condition) {
        if (condition.bad()) {
            cerr << condition.text() << " in " __FILE__ << ":" << __LINE__  << endl;
            throw OFConditionBadException();
        }
        return 0;
    }

}


