import os
import sys

LE = "\n"
INT = 0
UINT = 1
FLOAT = 2
INT64 = 3
UINT64 = 4
DOUBLE = 5
INT16 = 6
UINT16 = 7
FLOAT16 = 8
BOOL = 9

BIT_WIDTH_16 = 2
BIT_WIDTH_32 = 0
BIT_WIDTH_64 = 1

BIT_WIDTH_TYPES = ["i32", "i64", "i16"]

class ArithTypes():
    def __init__(self, GlslSTypes, GlslVTypes, MangleTypes, LlvmTypes):
        self.GlslSTypes = GlslSTypes
        self.GlslVTypes = GlslVTypes
        self.MangleTypes = MangleTypes
        self.LlvmTypes = LlvmTypes

ARITH_TYPES = [ ArithTypes("int", "ivec", "i", "i32"),
               ArithTypes("uint","uvec", "j", "i32"),
               ArithTypes("float", "vec", "f", "float"),
               ArithTypes("int64_t", "i64vec", "l", "i64"),
               ArithTypes("uint64_t", "u64vec", "m", "i64"),
               ArithTypes("double", "dvec", "d", "double"),
               ArithTypes("int16_t", "i16vec", "s", "i16"),
               ArithTypes("uint16_t", "u16vec", "t", "i16"),
               ArithTypes("float16_t", "f16vec", "Dh", "half"),
               ArithTypes("bool", "bvec", "b", "i1") ]

GROUP_OPERATION = ["sub_group_reduce", "sub_group_scan_inclusive", "sub_group_scan_exclusive"]
GROUP_FUNC_GLSL_NAME = [["subgroup", "subgroupInclusive", "subgroupExclusive"],
              ["InvocationsNonUniform", "InvocationsInclusiveScanNonUniform", "InvocationsExclusiveScanNonUniform"]]
GROUP_FUNC_LLVM_NAME = ["@llpc.subgroup.reduce.", "@llpc.subgroup.inclusiveScan.", "@llpc.subgroup.exclusiveScan."]

BINARY_OP_STRING = ["iadd", "imul", "smin", "smax", "umin","umax", "and", "or", "xor", "fmul", "fmin", "fmax", "fadd", "smin", "smax"]

SCALAR = 0
HIBYRID = 1

KHR = 0
AMD = 0

funcOps16 = [[{"add":(INT16, 0, HIBYRID)}, {"add":(FLOAT16,12,SCALAR)}],
           [{"min":(INT16,2, SCALAR)}, {"min":(UINT16,4, SCALAR)}, {"min":(FLOAT16,10, SCALAR)}],
           [{"max":(INT16,3, SCALAR)}, {"max":(UINT16,5, SCALAR)}, {"max":(FLOAT16,11, SCALAR)}]]

funcOps32 = [[{"add":(INT,0, HIBYRID)}, {"add":(FLOAT,12,SCALAR)}, {"add":(DOUBLE,12,SCALAR)}],
           [{"mul":(INT,1, HIBYRID)}, {"mul":(FLOAT,9,SCALAR)}, {"mul":(DOUBLE,9,SCALAR)}],
           [{"min":(INT,2, SCALAR)}, {"min":(UINT,4, SCALAR)}, {"min":(FLOAT,10,SCALAR)}, {"min":(DOUBLE,10,SCALAR)}],
           [{"max":(INT,3, SCALAR)}, {"max":(UINT,5, SCALAR)}, {"max":(FLOAT,11,SCALAR)}, {"max":(DOUBLE,11,SCALAR)}],
           [{"and":(INT,6, HIBYRID )}, {"and":(BOOL,6, SCALAR)}],
           [{"or":(INT,7, HIBYRID)}, {"or":(BOOL,7, SCALAR)}],
           [{"xor":(INT,8, HIBYRID)}, {"xor":(BOOL,8, SCALAR)}]]

funcOps64 = [[{"add":(INT64,0,HIBYRID)}],
           [{"min":(INT64,2,SCALAR)}, {"min":(UINT64,4,SCALAR)}],
           [ {"max":(INT64,3,SCALAR)}, {"max":(UINT64,5,SCALAR)}]]

funcOpsTable = {32:funcOps32, 16:funcOps16, 64:funcOps64}

def getCommentType(binType, vecNum, commentType):
    if commentType == SCALAR:
        if (vecNum == 0):
            return ARITH_TYPES[binType].GlslSTypes
        else:
            return ARITH_TYPES[binType].GlslVTypes + str(vecNum+1)
    else:
        outString = getCommentType(binType, vecNum, SCALAR) + "/" + getCommentType(binType + 1, vecNum, SCALAR)
        return outString

def getComment(binOp, binType, i, j, commentType, glslFuncType):
    glslTypeName = getCommentType(binType, j, commentType)
    outString = ""
    if (glslFuncType == KHR):
        outString = "; GLSL: " + glslTypeName + " " + GROUP_FUNC_GLSL_NAME[KHR][i] + binOp[0].upper() + binOp[1:] + "(" + glslTypeName + ")"
    else:
        outString = "; GLSL: " + glslTypeName + " " + binOp + GROUP_FUNC_GLSL_NAME[AMD][i] + "(" + glslTypeName + ")"
    outString += LE
    return outString

def getLlvmTypes(binType, j):
    if j == 0:
        return ARITH_TYPES[binType].LlvmTypes
    else:
        return "<" +str(j+1) + " x " + ARITH_TYPES[binType].LlvmTypes+ ">"

def mangle(binType, vecNum):
    if vecNum == 0:
        return ARITH_TYPES[binType].MangleTypes
    else:
        return "Dv" + str(vecNum+1) + "_" + ARITH_TYPES[binType].MangleTypes

def getFuncName(binOp, binType, groupScanType, vecNum, waveSize):
    llvmTypeName = getLlvmTypes(binType, vecNum)
    outString = GROUP_OPERATION[groupScanType] + "_" + binOp + "_nonuniform_wave"+str(waveSize)
    outString = "define spir_func " + llvmTypeName + " @_Z" + str(len(outString)) + outString + mangle(binType,vecNum) + "(" + llvmTypeName + " %value" + ")"
    outString += LE
    return outString

LINE_NUM = 0

def setLine(l):
    global LINE_NUM
    LINE_NUM = l

def getLine():
    return "%" + str(LINE_NUM)

def getLeftValue():
    global LINE_NUM

    outString = "    " + getLine() + " = "
    LINE_NUM = LINE_NUM + 1
    return outString

def getLastVar(var):
    lastLine = LINE_NUM - var - 2
    return "%" +str(lastLine)

def is16BitBinOp(binOp):
    return binOp == INT16 or binOp == UINT16

def is32BitBinOp(binOp):
    return binOp == INT or binOp == UINT

def is64BitBinOp(binOp):
    return binOp == INT64 or binOp == UINT64

def getFuncBody(binOp, binType, groupScanType, vecNum, binOpEnum, waveSize):

    groupFuncLlvmName = GROUP_FUNC_LLVM_NAME[groupScanType]
    if waveSize != 64:
        groupFuncLlvmName = groupFuncLlvmName + "wave" + str(waveSize)+"."

    binOpString = BINARY_OP_STRING[binOpEnum]
    binOpEnum = str(binOpEnum)
    sourceScalarType = ARITH_TYPES[binType].LlvmTypes
    llvmTypeName = getLlvmTypes(binType, vecNum)
    targetScalarType = ""
    targetType = ""
    bitWidth = 0

    if (binType == DOUBLE or binType == INT64 or binType == UINT64):
        targetScalarType = ARITH_TYPES[INT64].LlvmTypes
        targetType = getLlvmTypes(INT64, vecNum)
        bitWidth = BIT_WIDTH_64
    elif (binType == FLOAT or binType == INT or binType == UINT):
        targetScalarType = ARITH_TYPES[INT].LlvmTypes
        targetType = getLlvmTypes(INT, vecNum)
        bitWidth = BIT_WIDTH_32
    elif (binType == FLOAT16 or binType == INT16 or binType == UINT16):
        targetScalarType = ARITH_TYPES[INT].LlvmTypes
        targetType = getLlvmTypes(INT, vecNum)
        bitWidth = BIT_WIDTH_16
    elif (binType == BOOL):
        targetScalarType = ARITH_TYPES[INT].LlvmTypes
        targetType = getLlvmTypes(INT, vecNum)

    setLine(1)
    outString = "{"
    outString += LE
    outString += "    ; " + binOpEnum + " = arithmetic " + binOpString
    outString += LE

    lastVar = getLine()
    currVar = ""

    if (is32BitBinOp(binType) or is64BitBinOp(binType)):
        lastVar = "%value"
    else:
        lastVar = getLine()
        if (binType == FLOAT or binType == DOUBLE):
            outString += getLeftValue() + "bitcast " + llvmTypeName + " %value to " + targetType + LE
        elif (binType == BOOL):
            outString += getLeftValue() + "zext " + llvmTypeName + " %value to " + targetType + LE
        elif bitWidth == BIT_WIDTH_16:
            if (binType == UINT16):
                outString += getLeftValue() + "zext " + llvmTypeName + " %value to " + getLlvmTypes(binType - 6, vecNum) + LE
            elif (binType == INT16):
                outString += getLeftValue() + "sext " + llvmTypeName + " %value to " + getLlvmTypes(binType - 6, vecNum) + LE
            else:
                outString += getLeftValue() + "fpext " + llvmTypeName + " %value to " + getLlvmTypes(binType - 6, vecNum) + LE

            if (binType == FLOAT16):
                outString += getLeftValue() + "bitcast " + getLlvmTypes(binType - 6, vecNum) + " " + lastVar+ " to " +targetType + LE
                lastVar = getLastVar(-1)

    if (vecNum == 0):
        currVar = getLine()
        outString += getLeftValue() + "call {0} @llpc.subgroup.set.inactive.{3}(i32 {1}, {0} {2})". \
            format(targetScalarType, binOpEnum, lastVar, BIT_WIDTH_TYPES[bitWidth])
        lastVar = currVar
        outString += LE
        currVar = getLine()
        outString += getLeftValue() + "call {0} {1}{4}(i32 {2} ,{0} {3})".format(targetScalarType,
            groupFuncLlvmName, binOpEnum, lastVar, BIT_WIDTH_TYPES[bitWidth])
        outString += LE
        lastVar = currVar
    elif (vecNum > 0):
        outString += LE
        for i in range(vecNum + 1):
            outString += getLeftValue() + "extractelement " + targetType + " " + lastVar + ", i32 " +str(i) + LE
        outString += LE
        for i in range(vecNum + 1):
            outString += getLeftValue() + "call {0} @llpc.subgroup.set.inactive.{3}(i32 {1}, {0} {2})". \
                format(targetScalarType, binOpEnum, getLastVar(vecNum), BIT_WIDTH_TYPES[bitWidth]) + LE
        outString += LE

        for i in range(vecNum + 1):
            outString += getLeftValue() + "call {0} {1}{4}(i32 {2} ,{0} {3})".format(targetScalarType,
                groupFuncLlvmName, binOpEnum, getLastVar(vecNum), BIT_WIDTH_TYPES[bitWidth])
            outString += LE
        outString += LE
        for i in range(vecNum + 1):
            outString += getLeftValue() + "insertelement " + targetType + " "
            if i == 0:
                outString += "undef, {0} ".format(targetScalarType)
            else:
                outString += getLastVar(0) + ", {0} ".format(targetScalarType)
            outString += getLastVar(vecNum) + ", {0} ".format(targetScalarType) + str(i) + LE
        lastVar = getLastVar(-1)

    if (binType == FLOAT or binType == DOUBLE):
        currVar = getLine()
        outString += getLeftValue() + "bitcast " +targetType+ " " + lastVar + " to " + llvmTypeName
        lastVar = currVar
        outString += LE
    elif (binType == BOOL):
        currVar = getLine()
        outString += getLeftValue() + "trunc " +targetType+ " " + lastVar + " to " + llvmTypeName
        lastVar = currVar
        outString += LE
    elif (bitWidth == BIT_WIDTH_16):
        if binType == FLOAT16:
            currVar = getLine()
            outString += getLeftValue() + "bitcast " +targetType+ " " + lastVar + " to " + getLlvmTypes(binType - 6, vecNum)
            lastVar = currVar
            outString += LE

        if is16BitBinOp(binType):
            currVar = getLine()
            outString += getLeftValue() + "trunc " + getLlvmTypes(binType - 6, vecNum) + " " + lastVar + " to " + llvmTypeName
            lastVar = currVar
            outString += LE
        else:
            currVar = getLine()
            outString += getLeftValue() + "fptrunc " + getLlvmTypes(binType - 6, vecNum) + " " + lastVar + " to " + llvmTypeName
            lastVar = currVar
            outString += LE

    outString += LE
    outString += "    ret " + llvmTypeName + " " + lastVar
    outString += LE
    outString += "}"
    outString += LE
    return outString

def expandFunc(binOp, binType, binOpEnum, commentType, glslFuncType, waveSize):
    outString = ""
    for i in range(3):
        for j in range(4):
            outString += getComment(binOp, binType, i, j, commentType, glslFuncType)
            outString += getFuncName(binOp, binType, i, j, waveSize)
            outString += getFuncBody(binOp, binType, i, j, binOpEnum, waveSize)
            outString += LE
    return outString

def main(bitWidth, waveSize, directory):
    if not os.path.exists(directory):
        os.makedirs(directory)

    funcOps = funcOpsTable[bitWidth]
    outString = ""
    for i in funcOps:
        for j in i:
            for k in j.items():
                outString += expandFunc(k[0], k[1][0], k[1][1], k[1][2], KHR, waveSize)
    dataType = "{}bit".format(bitWidth)
    outFile = os.path.join(directory, "g_glslGroupOpEmuD{0}W{1}.ll".format(bitWidth, waveSize))
    irOut = open(outFile, "wt")
    header = """\
;**********************************************************************************************************************
;*
;*  Trade secret of Advanced Micro Devices, Inc.
;*  Copyright (c) 2018, Advanced Micro Devices, Inc., (unpublished)
;*
;*  All rights reserved. This notice is intended as a precaution against inadvertent publication and does not imply
;*  publication or any waiver of confidentiality. The year included in the foregoing notice is the year of creation of
;*  the work.
;*
;**********************************************************************************************************************

;**********************************************************************************************************************
;* @file  {0}
;* @brief LLPC LLVM-IR file: contains emulation codes for GLSL group operations ({1}).
;*
;* @note  This file has been generated automatically. Do not hand-modify this file. When changes are needed, modify the
;*        generating script genGlslGroupOpEmuCode.py.
;**********************************************************************************************************************

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-\
v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024"
target triple = "spir64-unknown-unknown"

"""

    tail = """\
declare spir_func i32 @llpc.subgroup.set.inactive.i32(i32, i32) #0
declare spir_func i32 @llpc.subgroup.reduce.i32(i32, i32) #0
declare spir_func i32 @llpc.subgroup.exclusiveScan.i32(i32, i32) #0
declare spir_func i32 @llpc.subgroup.inclusiveScan.i32(i32, i32) #0

declare spir_func i64 @llpc.subgroup.set.inactive.i64(i32, i64) #0
declare spir_func i64 @llpc.subgroup.reduce.i64(i32, i64) #0
declare spir_func i64 @llpc.subgroup.exclusiveScan.i64(i32, i64) #0
declare spir_func i64 @llpc.subgroup.inclusiveScan.i64(i32, i64) #0

declare spir_func i32 @llpc.subgroup.set.inactive.i16(i32, i32) #0
declare spir_func i32 @llpc.subgroup.reduce.i16(i32, i32) #0
declare spir_func i32 @llpc.subgroup.exclusiveScan.i16(i32, i32) #0
declare spir_func i32 @llpc.subgroup.inclusiveScan.i16(i32, i32) #0

attributes #0 = { nounwind }
"""

    header = header.format(outFile, dataType)
    written = irOut.write(header)
    written = irOut.write(outString)
    written = irOut.write(tail)
    irOut.close()

if __name__ == "__main__":
    bitWidth = int(sys.argv[1])
    wave = int(sys.argv[2])
    main(bitWidth, wave, sys.argv[3])
