#include "CliCeLoginV2.h"
#include "CliTypes.h"

#include "../celogin/src/CeLoginAsnV1.h"
#include "../celogin/src/CeLoginJson.h"
#include "../celogin/src/CeLoginUtil.h"
#include "CliUtils.h"

#include <CeLogin.h>
#include <inttypes.h>
#include <json-c/json.h>
#include <openssl/asn1.h>
#include <openssl/asn1t.h>
#include <openssl/obj_mac.h>
#include <openssl/objects.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/x509.h> // Needed for reading in public key
#include <string.h>
#include <unistd.h>

#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <vector>

using CeLogin::CeLoginRc;
using CeLogin::CeLoginCreateHsfArgsV1;
using CeLogin::CeLoginCreateHsfArgsV2;

CeLoginRc CeLogin::createCeLoginAcfV2Payload(
    const CeLoginCreateHsfArgsV2& argsParm, std::string& generatedJsonParm,
    std::vector<uint8_t>& generatedPayloadHashParm)
{
    CeLoginRc sRc = CeLoginRc::Success;

    const CeLoginCreateHsfArgsV2& sArgsV2 = argsParm;
    const CeLoginCreateHsfArgsV1& sArgsV1 = argsParm.mV1Args;

    std::string sPasswordHashHexString;
    std::string sSaltHexString;
    std::string sReplayId = cli::generateReplayId();
    const bool sIsAdminReset = ("adminreset" == sArgsV2.mType);

    std::vector<uint8_t> sHashedAuthCode(sArgsV1.mHashedAuthCodeLength);
    std::vector<uint8_t> sSalt(sArgsV1.mSaltLength, 0);

    uint64_t sIterations = sArgsV1.mIterations;

    if (sArgsV1.mMachines.empty() || !sArgsV1.mPasswordPtr ||
        0 == sArgsV1.mPasswordLength || sArgsV1.mExpirationDate.empty() ||
        sArgsV1.mRequestId.empty() || sArgsV2.mType.empty())
    {
        sRc = CeLoginRc::Failure;
        std::cout << "ERROR line " << __LINE__ << std::endl;
    }

    if (CeLoginRc::Success == sRc &&
        PasswordHash_Production == sArgsV1.mPasswordHashAlgorithm)
    {
        // Create a random salt
        int sOsslRc = RAND_bytes(sSalt.data(), sSalt.size());
        if (1 != sOsslRc)
        {
            sRc = CeLoginRc::Failure;
        }
    }

    // Hash password
    if (CeLoginRc::Success == sRc)
    {
        if (PasswordHash_Production == sArgsV1.mPasswordHashAlgorithm)
        {
            sRc = CeLogin::createPasswordHash(
                sArgsV1.mPasswordPtr, sArgsV1.mPasswordLength, sSalt.data(),
                sSalt.size(), sIterations, sHashedAuthCode.data(),
                sHashedAuthCode.size(), sHashedAuthCode.size());
        }
        else if (PasswordHash_SHA512 == sArgsV1.mPasswordHashAlgorithm)
        {
            sIterations = 0;
            bool sSuccess = cli::createSha512PasswordHash(
                (const uint8_t*)sArgsV1.mPasswordPtr, sArgsV1.mPasswordLength,
                sHashedAuthCode);
            if (!sSuccess)
            {
                sRc = CeLoginRc::Failure;
            }
        }
        else
        {
            std::cout << "Error, unrecognized hash algorithm for password"
                      << std::endl;
            sRc = CeLoginRc::Failure;
        }
    }

    // Convert binary hash to Hex String
    if (CeLoginRc::Success == sRc)
    {
        sPasswordHashHexString = cli::getHexStringFromBinary(sHashedAuthCode);
        sSaltHexString = cli::getHexStringFromBinary(sSalt);
    }

    // Create json structure
    if (CeLoginRc::Success == sRc)
    {
        json_object* sJsonObj = json_object_new_object();
        json_object* sVersion = json_object_new_int(CeLoginVersion2);
        json_object* sMachinesArray = json_object_new_array();

        json_object* sHashedPassword =
            json_object_new_string(sPasswordHashHexString.c_str());
        json_object* sSaltObj = json_object_new_string(sSaltHexString.c_str());
        json_object* sIterationsObj = json_object_new_int(sIterations);
        json_object* sExpirationDate =
            json_object_new_string(sArgsV1.mExpirationDate.c_str());
        json_object* sRequestId =
            json_object_new_string(sArgsV1.mRequestId.c_str());
        json_object* sReplayIdJson = json_object_new_string(sReplayId.c_str());
        json_object* sAdminAuthCodeJson = nullptr;
        json_object* sAcfTypeJson = json_object_new_string(sArgsV2.mType.c_str());

        if (sJsonObj && sVersion && sMachinesArray && sHashedPassword &&
            sExpirationDate && sRequestId && sReplayIdJson && sAcfTypeJson)
        {
            for (int sIdx = 0; sIdx < sArgsV1.mMachines.size(); sIdx++)
            {
                json_object* sMachinesObj = json_object_new_object();
                json_object* sSerialNumber = json_object_new_string(
                    sArgsV1.mMachines[sIdx].mSerialNumber.c_str());
                std::string sFrameworkEcStr;

                if (cli::P10 == sArgsV1.mMachines[sIdx].mProc)
                {
                    if (ServiceAuth_Dev == sArgsV1.mMachines[sIdx].mAuth)
                    {
                        sFrameworkEcStr = FrameworkEc_P10_Dev;
                    }
                    else if (ServiceAuth_CE == sArgsV1.mMachines[sIdx].mAuth)
                    {
                        sFrameworkEcStr = FrameworkEc_P10_Service;
                    }
                }

                json_object* sFrameworkEc =
                    json_object_new_string(sFrameworkEcStr.c_str());

                if (sFrameworkEc && sSerialNumber && sMachinesObj)
                {
                    json_object_object_add(sMachinesObj, JsonName_SerialNumber,
                                           sSerialNumber);
                    json_object_object_add(sMachinesObj, JsonName_FrameworkEc,
                                           sFrameworkEc);
                    json_object_array_add(sMachinesArray, sMachinesObj);
                }
                else
                {
                    if (sMachinesObj)
                        json_object_put(sMachinesObj);
                    if (sFrameworkEc)
                        json_object_put(sFrameworkEc);
                    if (sSerialNumber)
                        json_object_put(sSerialNumber);
                    sRc = CeLoginRc::Failure;
                    break;
                }
            }

            json_object_object_add(sJsonObj, JsonName_Version, sVersion);
            json_object_object_add(sJsonObj, JsonName_Type, sAcfTypeJson);
            json_object_object_add(sJsonObj, JsonName_Machines, sMachinesArray);

            if(sIsAdminReset)
            {
                std::string sAdminAuthCode;
                if(cli::generateEtcPasswdHash(sArgsV1.mPasswordPtr, sArgsV1.mPasswordLength,
                                              sSaltHexString, sAdminAuthCode))
                {
                    std::vector<uint8_t> sAuthCodeBytes(sAdminAuthCode.begin(), sAdminAuthCode.end());
                    std::string sHexEncodedAuthCode = cli::getHexStringFromBinary(sAuthCodeBytes);

                    sAdminAuthCodeJson = json_object_new_string(sHexEncodedAuthCode.c_str());
                    if(NULL != sAdminAuthCodeJson)
                    {
                        json_object_object_add(sJsonObj, JsonName_AdminAuthCode, sAdminAuthCodeJson);
                    }
                    else { sRc = CeLoginRc::Failure; }
                }
                else { sRc = CeLoginRc::Failure; }
            }
            else // service type
            {
                json_object_object_add(sJsonObj, JsonName_HashedAuthCode, sHashedPassword);
                json_object_object_add(sJsonObj, JsonName_Salt, sSaltObj);
                json_object_object_add(sJsonObj, JsonName_Iterations, sIterationsObj);
            }

            json_object_object_add(sJsonObj, JsonName_RequestId, sRequestId);

            if(!sArgsV2.mNoReplayId)
            {
                json_object_object_add(sJsonObj, JsonName_ReplayId, sReplayIdJson);
            }

            json_object_object_add(sJsonObj, JsonName_Expiration, sExpirationDate);

            // When the json object is free'd this string will also be free'd
            const char* sGeneratedJsonString =
                json_object_to_json_string(sJsonObj);

            if (sGeneratedJsonString)
            {
                generatedJsonParm = std::string(sGeneratedJsonString);
            }
            else
            {
                sRc = CeLoginRc::Failure;
            }
        }
        else
        {
            sRc = CeLoginRc::Failure;
        }

        if (CeLoginRc::Success != sRc)
        {
            // deallocate memory
            if (sJsonObj)
            {
                json_object_put(sJsonObj);
                sJsonObj = NULL;
            }
            if (sVersion)
            {
                json_object_put(sVersion);
                sVersion = NULL;
            }
            if (sMachinesArray)
            {
                json_object_put(sMachinesArray);
                sMachinesArray = NULL;
            }
            if (sHashedPassword)
            {
                json_object_put(sHashedPassword);
                sHashedPassword = NULL;
            }
            if (sExpirationDate)
            {
                json_object_put(sExpirationDate);
                sExpirationDate = NULL;
            }
            if (sRequestId)
            {
                json_object_put(sRequestId);
                sRequestId = NULL;
            }
            if(sReplayIdJson)
            {
                json_object_put(sReplayIdJson);
                sReplayIdJson = NULL;
            }
            if(sAdminAuthCodeJson)
            {
                json_object_put(sAdminAuthCodeJson);
                sAdminAuthCodeJson = NULL;
            }
            if(sAcfTypeJson)
            {
                json_object_put(sAcfTypeJson);
                sAcfTypeJson = NULL;
            }
        }

        if (sJsonObj)
        {
            json_object_put(sJsonObj);
            sJsonObj = NULL;
        }
    }

    if (CeLoginRc::Success == sRc && !generatedJsonParm.empty())
    {
        generatedPayloadHashParm =
            std::vector<uint8_t>(CeLogin::CeLogin_DigestLength);

        sRc = createDigest((const uint8_t*)generatedJsonParm.data(),
                           generatedJsonParm.length(),
                           generatedPayloadHashParm.data(),
                           generatedPayloadHashParm.size());
    }

    if (CeLoginRc::Success != sRc)
    {
        generatedPayloadHashParm.clear();
        generatedJsonParm.clear();
    }
    return sRc;
}

CeLogin::CeLoginRc CeLogin::createCeLoginAcfV2Signature(
    const CeLoginCreateHsfArgsV2& argsParm,
    const std::vector<uint8_t>& jsonDigestParm,
    std::vector<uint8_t>& generatedSignatureParm)
{
    CeLoginRc sRc = CeLoginRc::Success;

    const CeLoginCreateHsfArgsV2& sArgsV2 = argsParm;
    const CeLoginCreateHsfArgsV1& sArgsV1 = argsParm.mV1Args;

    const uint8_t* sConstPrivateKey = sArgsV1.mPrivateKey.data();
    RSA* sPrivateKey =
        d2i_RSAPrivateKey(NULL, &sConstPrivateKey, sArgsV1.mPrivateKey.size());
    // TODO: Verify size matches expected size
    if (sPrivateKey)
    {
        unsigned int sJsonSignatureSize = 0;
        generatedSignatureParm = std::vector<uint8_t>(RSA_size(sPrivateKey));
        int sResult =
            RSA_sign(CeLogin::CeLogin_Digest_NID, jsonDigestParm.data(),
                     jsonDigestParm.size(), generatedSignatureParm.data(),
                     &sJsonSignatureSize, sPrivateKey);
        if (1 != sResult)
        {
            sRc = CeLoginRc::Failure;
        }
        else if (sJsonSignatureSize != generatedSignatureParm.size())
        {
            sRc = CeLoginRc::Failure;
        }
    }
    else
    {
        sRc = CeLoginRc::Failure;
        std::cout << "huh, that's odd" << std::endl;
    }

    if (sPrivateKey)
    {
        RSA_free(sPrivateKey);
    }

    if (sRc != CeLoginRc::Success)
    {
        generatedSignatureParm.clear();
    }

    return sRc;
}

CeLogin::CeLoginRc
    CeLogin::createCeLoginAcfV2Asn1(const CeLoginCreateHsfArgsV2& argsParm,
                                    const std::string& jsonParm,
                                    const std::vector<uint8_t>& signatureParm,
                                    std::vector<uint8_t>& generatedAcfParm)
{
    CeLoginRc sRc = CeLoginRc::Success;

    const CeLoginCreateHsfArgsV2& sArgsV2 = argsParm;
    const CeLoginCreateHsfArgsV1& sArgsV1 = argsParm.mV1Args;
    CELoginSequenceV1* sHsfStruct = NULL;

    if (sArgsV1.mSourceFileName.empty() || jsonParm.empty() ||
        signatureParm.empty())
    {
        sRc = CeLoginRc::Failure;
        std::cout << "ERROR line " << __LINE__ << std::endl;
    }

    if (CeLoginRc::Success == sRc)
    {
        sHsfStruct = CELoginSequenceV1_new();

        ASN1_STRING_set(sHsfStruct->processingType, CeLogin::AcfProcessingType,
                        strlen(CeLogin::AcfProcessingType));
        ASN1_STRING_set(sHsfStruct->sourceFileName,
                        sArgsV1.mSourceFileName.c_str(),
                        sArgsV1.mSourceFileName.size());
        ASN1_OCTET_STRING_set(sHsfStruct->sourceFileData,
                              (const uint8_t*)jsonParm.data(), jsonParm.size());
        sHsfStruct->algorithm->id = OBJ_nid2obj(CeLogin::CeLogin_Acf_NID);
        ASN1_BIT_STRING_set(sHsfStruct->signature,
                            (uint8_t*)signatureParm.data(),
                            signatureParm.size());

        std::vector<uint8_t> sHsfDerEncoded(4096);
        uint8_t* sDataPtr = sHsfDerEncoded.data();
        uint64_t sHsfDerEncodedLength =
            i2d_CELoginSequenceV1(sHsfStruct, &sDataPtr);
        if (sHsfDerEncodedLength > 0)
        {
            // Remove extra data from vector
            sHsfDerEncoded.erase(sHsfDerEncoded.begin() + sHsfDerEncodedLength,
                                 sHsfDerEncoded.end());
            generatedAcfParm = sHsfDerEncoded;
        }
        else
        {
            sRc = CeLoginRc::Failure;
        }

        CELoginSequenceV1_free(sHsfStruct);
    }

    if (sRc != CeLoginRc::Success)
    {
        generatedAcfParm.clear();
    }

    return sRc;
}

CeLogin::CeLoginRc
    CeLogin::createCeLoginAcfV2(const CeLoginCreateHsfArgsV2& argsParm,
                                std::vector<uint8_t>& generatedAcfParm)
{
    std::string sJsonString;
    std::vector<uint8_t> sJsonDigest;
    CeLoginRc sRc =
        createCeLoginAcfV2Payload(argsParm, sJsonString, sJsonDigest);

    std::vector<uint8_t> sJsonSignature;

    if (CeLoginRc::Success == sRc)
    {
        sRc =
            createCeLoginAcfV2Signature(argsParm, sJsonDigest, sJsonSignature);
    }

    if (CeLoginRc::Success == sRc)
    {
        sRc = createCeLoginAcfV2Asn1(argsParm, sJsonString, sJsonSignature,
                                     generatedAcfParm);
    }

    return sRc;
}

CeLogin::CeLoginRc CeLogin::decodeAndVerifyCeLoginHsfV2(
    const std::vector<uint8_t>& hsfParm,
    const std::vector<uint8_t>& publicKeyParm,
    CeLoginDecryptedHsfArgsV2& decodedHsfParm)
{
    CeLoginRc sRc = CeLoginRc::Success;

    CELoginSequenceV1* sDecodedAsn = NULL;

    if (CeLoginRc::Success == sRc)
    {
        if (!publicKeyParm.empty())
        {
            sRc = decodeAndVerifyAcf(hsfParm.data(), hsfParm.size(),
                                     publicKeyParm.data(), publicKeyParm.size(),
                                     sDecodedAsn);
        }
        else
        {
            if (hsfParm.empty())
            {
                sRc = CeLoginRc::VerifyAcf_InvalidParm;
            }

            if (CeLoginRc::Success == sRc)
            {
                // return a valid TYPE structure or NULL if an error occurs
                // NOTE: there is a "reuse" capability where an existing
                // structure can be provided,
                //       however in the event of a failure, the structure is
                //       automatically free'd. Either way there is undesirable
                //       behavior. So in this case returning a heap allocation
                //       seems slightly more straightforward.
                const uint8_t* sHsfPtr = hsfParm.data();
                sDecodedAsn =
                    d2i_CELoginSequenceV1(NULL, &sHsfPtr, hsfParm.size());
                if (!sDecodedAsn)
                {
                    sRc = CeLoginRc::VerifyAcf_AsnDecodeFailure;
                }
            }
        }
    }

    if (CeLoginRc::Success == sRc)
    {
        decodedHsfParm.mProcessingType =
            std::string((const char*)sDecodedAsn->processingType->data,
                        sDecodedAsn->processingType->length);
        decodedHsfParm.mSourceFileName =
            std::string((const char*)sDecodedAsn->sourceFileName->data,
                        sDecodedAsn->sourceFileName->length);
    }

    if (CeLoginRc::Success == sRc)
    {
        for (int i = 0; i < sDecodedAsn->sourceFileData->length; i++)
        {
            decodedHsfParm.mSignedPayload.push_back(
                sDecodedAsn->sourceFileData->data[i]);
        }

        for (int i = 0; i < sDecodedAsn->signature->length; i++)
        {
            decodedHsfParm.mSignature.push_back(
                sDecodedAsn->signature->data[i]);
        }
        json_object* sJson =
            json_tokener_parse((const char*)sDecodedAsn->sourceFileData->data);

        if (!sJson)
        {
            sRc = CeLoginRc::DecodeHsf_JsonParseFailure;
        }

        int32_t sVersion = 0;
        if (CeLoginRc::Success == sRc &&
            cli::getIntFromJson(sJson, CeLogin::JsonName_Version, sVersion))
        {
            if (CeLoginVersion2 != sVersion)
            {
                sRc = CeLoginRc::DecodeHsf_VersionMismatch;
            }
        }

        // JSW TODO This won't work without some modification to handle types
        if (CeLoginRc::Success == sRc)
        {
            json_object* sMachinesArray = NULL;
            json_object* sMachinesObj = NULL;
            bool sMachineResult = json_object_object_get_ex(
                sJson, CeLogin::JsonName_Machines, &sMachinesArray);
            if (sMachineResult && sMachinesArray)
            {
                const size_t sArrayLength =
                    json_object_array_length(sMachinesArray);
                for (size_t sIdx = 0; sIdx < sArrayLength; sIdx++)
                {
                    sMachinesObj =
                        json_object_array_get_idx(sMachinesArray, sIdx);
                    if (sMachinesObj)
                    {
                        DecodedMachine sMachineEntry;
                        if (CeLoginRc::Success == sRc &&
                            !cli::getStringFromJson(
                                sMachinesObj, CeLogin::JsonName_SerialNumber,
                                sMachineEntry.mSerialNumber))
                        {
                            sRc = CeLoginRc::DecodeHsf_ReadSerialNumberFailure;
                        }
                        if (CeLoginRc::Success == sRc &&
                            !cli::getStringFromJson(
                                sMachinesObj, CeLogin::JsonName_FrameworkEc,
                                sMachineEntry.mFrameworkEc))
                        {
                            sRc = CeLoginRc::DecodeHsf_ReadFrameworkEcFailure;
                        }
                        if (CeLoginRc::Success == sRc)
                        {
                            decodedHsfParm.mMachines.push_back(sMachineEntry);
                        }
                        else
                        {
                            break;
                        }
                    }
                }
            }
            else
            {
                sRc = CeLoginRc::DecodeHsf_ReadMachineArrayFailure;
            }
        }

        if (CeLoginRc::Success == sRc &&
            !cli::getStringFromJson(sJson, CeLogin::JsonName_HashedAuthCode,
                                    decodedHsfParm.mPasswordHash))
        {
            sRc = CeLoginRc::DecodeHsf_ReadHashedAuthCodeFailure;
        }

        if (CeLoginRc::Success == sRc &&
            !cli::getStringFromJson(sJson, CeLogin::JsonName_Salt,
                                    decodedHsfParm.mSalt))
        {
            sRc = CeLoginRc::DecodeHsf_ReadSaltFailure;
        }

        if (CeLoginRc::Success == sRc &&
            !cli::getIntFromJson(sJson, CeLogin::JsonName_Iterations,
                                 decodedHsfParm.mIterations))
        {
            sRc = CeLoginRc::DecodeHsf_ReadIterationsFailure;
        }

        if (CeLoginRc::Success == sRc &&
            !cli::getStringFromJson(sJson, CeLogin::JsonName_Expiration,
                                    decodedHsfParm.mExpirationDate))
        {
            sRc = CeLoginRc::DecodeHsf_ReadExpirationFailure;
        }

        if (CeLoginRc::Success == sRc &&
            !cli::getStringFromJson(sJson, CeLogin::JsonName_RequestId,
                                    decodedHsfParm.mRequestId))
        {
            sRc = CeLoginRc::DecodeHsf_ReadRequestIdFailure;
        }

        if (sJson)
        {
            json_object_put(sJson);
        }
    }

    if (sDecodedAsn)
    {
        CELoginSequenceV1_free(sDecodedAsn);
    }
    return sRc;
}
