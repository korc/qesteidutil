/*
 * QDigiDocCommon
 *
 * Copyright (C) 2010-2011 Jargo Kõster <jargo@innovaatik.ee>
 * Copyright (C) 2010-2011 Raul Metsma <raul@innovaatik.ee>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "QPKCS11_p.h"

#include "PinDialog.h"

#include <QApplication>
#include <QStringList>

#include <openssl/evp.h>

QPKCS11Private::QPKCS11Private()
: f(0)
, pslot(0)
, pslots(0)
, session(0)
, nslots(0)
{
	memcpy( &method, RSA_get_default_method(), sizeof(method) );
	method.name = "QPKCS11";
	method.rsa_sign = rsa_sign;
}

bool QPKCS11Private::attribute( CK_OBJECT_HANDLE obj, CK_ATTRIBUTE_TYPE type, void *value, unsigned long &size ) const
{
	CK_ATTRIBUTE attr = { type, value, size };
	CK_RV err = f->C_GetAttributeValue( session, obj, &attr, 1 );
	size = attr.ulValueLen;
	return err == CKR_OK;
}

QByteArray QPKCS11Private::attribute( CK_OBJECT_HANDLE obj, CK_ATTRIBUTE_TYPE type ) const
{
	unsigned long size = 0;
	if( !attribute( obj, type, 0, size ) )
		return QByteArray();
	QByteArray data;
	data.resize( size );
	if( !attribute( obj, type, data.data(), size ) )
		return QByteArray();
	return data;
}

BIGNUM* QPKCS11Private::attribute_bn( CK_OBJECT_HANDLE obj, CK_ATTRIBUTE_TYPE type ) const
{
	QByteArray data = attribute( obj, type );
	return BN_bin2bn( (const unsigned char*)data.constData(), data.size(), 0 );
}

QSslCertificate QPKCS11Private::readCert( CK_SLOT_ID slot )
{
	if( session )
		f->C_CloseSession( session );
	session = 0;
	if( f->C_OpenSession( slot, CKF_SERIAL_SESSION, 0, 0, &session ) != CKR_OK )
		return QSslCertificate();

	CK_OBJECT_HANDLE obj = CK_INVALID_HANDLE;
	if( !findObject( CKO_CERTIFICATE, &obj ) || obj == CK_INVALID_HANDLE )
		return QSslCertificate();

	return QSslCertificate( attribute( obj, CKA_VALUE ), QSsl::Der );
}

bool QPKCS11Private::findObject( CK_OBJECT_CLASS cls, CK_OBJECT_HANDLE *ret ) const
{
	*ret = CK_INVALID_HANDLE;

	CK_ATTRIBUTE attr = { CKA_CLASS, &cls, sizeof(cls) };
	if( f->C_FindObjectsInit( session, &attr, 1 ) != CKR_OK )
		return false;

	CK_ULONG count = 0;
	CK_RV err = f->C_FindObjects( session, ret, 1, &count );
	f->C_FindObjectsFinal( session );
	return err == CKR_OK;
}

void QPKCS11Private::freeSlotIds()
{
	delete [] pslots;
	pslots = 0;
	nslots = 0;
}

bool QPKCS11Private::getSlotsIds()
{
	freeSlotIds();
	if( f->C_GetSlotList( true, 0, &nslots ) != CKR_OK )
		return false;
	pslots = new CK_SLOT_ID[nslots];
	if( f->C_GetSlotList( true, pslots, &nslots ) == CKR_OK )
		return true;
	freeSlotIds();
	return false;
}

int QPKCS11Private::rsa_sign( int type, const unsigned char *m, unsigned int m_len,
	unsigned char *sigret, unsigned int *siglen, const RSA *rsa )
{
	if( type != NID_md5_sha1 && m_len != 36 ) //ssl
		return 0;
	QPKCS11 *d = (QPKCS11*)RSA_get_app_data( rsa );
	QByteArray sig = d->sign( type, QByteArray( (char*)m, m_len ) );
	if( sig.isEmpty() )
		return 0;
	*siglen = sig.size();
	qMemCopy( sigret, sig.constData(), sig.size() );
	return 1;
}



QPKCS11::QPKCS11( QObject *parent )
:	QObject( parent )
,	d( new QPKCS11Private )
{
}

QPKCS11::~QPKCS11() { unloadDriver(); delete d; }

QStringList QPKCS11::cards()
{
	if( !d->getSlotsIds() )
		return QStringList();

	QStringList cards;
	for( unsigned long i = 0; i < d->nslots; ++i )
	{
		CK_TOKEN_INFO token;
		d->f->C_GetTokenInfo( d->pslots[i], &token );
		cards << QByteArray( (const char*)token.serialNumber, 16 ).trimmed();
	}
	cards.removeDuplicates();
	qSort( cards.begin(), cards.end(), qGreater<QString>() );
	return cards;
}

QByteArray QPKCS11::encrypt( const QByteArray &data ) const
{
	CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
	if( !d->findObject( CKO_PRIVATE_KEY, &key ) || key == CK_INVALID_HANDLE )
		return QByteArray();

	CK_MECHANISM mech = { CKM_RSA_PKCS, 0, 0 };
	if( d->f->C_EncryptInit( d->session, &mech, key ) != CKR_OK )
		return QByteArray();

	unsigned long size = 0;
	if( d->f->C_Encrypt( d->session, (unsigned char*)data.constData(), data.size(), 0, &size ) != CKR_OK )
		return QByteArray();

	QByteArray result;
	result.resize( size );
	if( d->f->C_Encrypt( d->session, (unsigned char*)data.constData(), data.size(), (unsigned char*)result.data(), &size ) != CKR_OK )
		return QByteArray();
	return result;
}

QString QPKCS11::errorString( PinStatus error )
{
	switch( error )
	{
	case QPKCS11::PinOK: return QString();
	case QPKCS11::PinCanceled: return tr("PIN Canceled");
	case QPKCS11::PinLocked: return tr("PIN locked");
	case QPKCS11::PinIncorrect: return tr("PIN Incorrect");
	case QPKCS11::GeneralError: return tr("PKCS11 general error");
	case QPKCS11::DeviceError: return tr("PKCS11 device error");
	default: return tr("PKCS11 unknown error");
	}
}

QByteArray QPKCS11::decrypt( const QByteArray &data ) const
{
	CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
	if( !d->findObject( CKO_PRIVATE_KEY, &key ) || key == CK_INVALID_HANDLE )
		return QByteArray();

	CK_MECHANISM mech = { CKM_RSA_PKCS, 0, 0 };
	if( d->f->C_DecryptInit( d->session, &mech, key ) != CKR_OK )
		return QByteArray();

	unsigned long size = 0;
	if( d->f->C_Decrypt( d->session, (unsigned char*)data.constData(), data.size(), 0, &size ) != CKR_OK )
		return QByteArray();

	QByteArray result;
	result.resize( size );
	if( d->f->C_Decrypt( d->session, (unsigned char*)data.constData(), data.size(), (unsigned char*)result.data(), &size ) != CKR_OK )
		return QByteArray();
	return result;
}

Qt::HANDLE QPKCS11::key()
{
	CK_OBJECT_HANDLE obj = CK_INVALID_HANDLE;
	if( !d->findObject( CKO_PRIVATE_KEY, &obj ) || obj == CK_INVALID_HANDLE )
		return false;

	RSA *rsa = RSA_new();
	if( !(rsa->n = d->attribute_bn( obj, CKA_MODULUS )) ||
		!(rsa->e = d->attribute_bn( obj, CKA_PUBLIC_EXPONENT )) )
		return 0;

	RSA_set_method( rsa, &(d->method) );
	rsa->flags |= RSA_FLAG_SIGN_VER;
	RSA_set_app_data( rsa, this );
	EVP_PKEY *key = EVP_PKEY_new();
	EVP_PKEY_set1_RSA( key, rsa );
	return Qt::HANDLE(key);
}

bool QPKCS11::loadDriver( const QString &driver )
{
	d->lib.setFileName( driver );
	CK_C_GetFunctionList l = CK_C_GetFunctionList(d->lib.resolve( "C_GetFunctionList" ));
	if( !l )
		return false;

	if( l( &d->f ) != CKR_OK )
		return false;

	return d->f->C_Initialize( 0 ) == CKR_OK;
}

QPKCS11::PinStatus QPKCS11::login( const TokenData &_t )
{
	CK_TOKEN_INFO token;
	if( !d->pslot || d->f->C_GetTokenInfo( *(d->pslot), &token ) != CKR_OK )
		return UnknownError;

	if( !(token.flags & CKF_LOGIN_REQUIRED) )
		return PinOK;

	TokenData t = _t;
	if( token.flags & CKF_SO_PIN_COUNT_LOW || token.flags & CKF_USER_PIN_COUNT_LOW )
		t.setFlag( TokenData::PinCountLow );
	if( token.flags & CKF_SO_PIN_FINAL_TRY || token.flags & CKF_USER_PIN_FINAL_TRY )
		t.setFlag( TokenData::PinFinalTry );
	if( token.flags & CKF_SO_PIN_LOCKED || token.flags & CKF_USER_PIN_LOCKED )
		t.setFlag( TokenData::PinLocked );

	if( d->session )
		d->f->C_CloseSession( d->session );
	d->session = 0;
	if( d->f->C_OpenSession( *(d->pslot), CKF_SERIAL_SESSION, 0, 0, &d->session ) != CKR_OK )
		return UnknownError;

	CK_RV err = CKR_OK;
	bool pin2 = SslCertificate( t.cert() ).keyUsage().keys().contains( SslCertificate::NonRepudiation );
	if( token.flags & CKF_PROTECTED_AUTHENTICATION_PATH )
	{
		PinDialog p( pin2 ? PinDialog::Pin2PinpadType : PinDialog::Pin1PinpadType, t, qApp->activeWindow() );
		QPKCS11Thread t( d );
		connect( &t, SIGNAL(started()), &p, SIGNAL(startTimer()) );
		p.open();
		err = t.waitForDone();
	}
	else
	{
		PinDialog p( pin2 ? PinDialog::Pin2Type : PinDialog::Pin1Type, t, qApp->activeWindow() );
		if( !p.exec() )
			return PinCanceled;
		QByteArray pin = p.text().toUtf8();
		err = d->f->C_Login( d->session, CKU_USER, (unsigned char*)pin.constData(), pin.size() );
	}

	switch( err )
	{
	case CKR_OK:
	case CKR_USER_ALREADY_LOGGED_IN:
		return PinOK;
	case CKR_CANCEL:
	case CKR_FUNCTION_CANCELED: return PinCanceled;
	case CKR_PIN_INCORRECT: return PinIncorrect;
	case CKR_PIN_LOCKED: return PinLocked;
	case CKR_DEVICE_ERROR: return DeviceError;
	case CKR_GENERAL_ERROR: return GeneralError;
	default: return UnknownError;
	}
}

bool QPKCS11::logout() const { return d->f->C_Logout( d->session ) == CKR_OK; }

TokenData QPKCS11::selectSlot( const QString &card, SslCertificate::KeyUsage usage )
{
	delete d->pslot;
	d->pslot = 0;
	TokenData t;
	t.setCard( card );
	for( unsigned int i = 0; i < d->nslots; ++i )
	{
		CK_TOKEN_INFO token;
		if( d->f->C_GetTokenInfo( d->pslots[i], &token ) != CKR_OK ||
			card != QByteArray( (const char*)token.serialNumber, 16 ).trimmed() )
			continue;

		SslCertificate cert = d->readCert( d->pslots[i] );
		if( !cert.keyUsage().keys().contains( usage ) )
			continue;

		d->pslot = new CK_SLOT_ID( d->pslots[i] );
		t.setCert( cert );
		if( token.flags & CKF_SO_PIN_COUNT_LOW || token.flags & CKF_USER_PIN_COUNT_LOW )
			t.setFlag( TokenData::PinCountLow );
		if( token.flags & CKF_SO_PIN_FINAL_TRY || token.flags & CKF_USER_PIN_FINAL_TRY )
			t.setFlag( TokenData::PinFinalTry );
		if( token.flags & CKF_SO_PIN_LOCKED || token.flags & CKF_USER_PIN_LOCKED )
			t.setFlag( TokenData::PinLocked );
		break;
	}
	return t;
}

QByteArray QPKCS11::sign( int type, const QByteArray &digest ) const
{
	QByteArray data;
	switch( type )
	{
	case NID_sha1: data += QByteArray::fromHex("3021300906052b0e03021a05000414"); break;
	case NID_sha224: data += QByteArray::fromHex("302d300d06096086480165030402040500041c"); break;
	case NID_sha256: data += QByteArray::fromHex("3031300d060960864801650304020105000420"); break;
	case NID_sha384: data += QByteArray::fromHex("3041300d060960864801650304020205000430"); break;
	case NID_sha512: data += QByteArray::fromHex("3051300d060960864801650304020305000440"); break;
	default: break;
	}
	data.append( digest );

	CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
	if( !d->findObject( CKO_PRIVATE_KEY, &key ) || key == CK_INVALID_HANDLE )
		return QByteArray();

	CK_MECHANISM mech = { CKM_RSA_PKCS, 0, 0 };
	if( d->f->C_SignInit( d->session, &mech, key ) != CKR_OK )
		return QByteArray();

	unsigned long size = 0;
	if( d->f->C_Sign( d->session, (unsigned char*)data.constData(),
			data.size(), 0, &size ) != CKR_OK )
		return QByteArray();

	QByteArray sig;
	sig.resize( size );
	if( d->f->C_Sign( d->session, (unsigned char*)data.constData(),
			data.size(), (unsigned char*)sig.data(), &size ) != CKR_OK )
		return QByteArray();
	return sig;
}

void QPKCS11::unloadDriver()
{
	d->freeSlotIds();
	delete d->pslot;
	d->pslot = 0;
	if( d->f )
		d->f->C_Finalize( 0 );
	d->f = 0;
	d->lib.unload();
}

bool QPKCS11::verify( const QByteArray &data, const QByteArray &signature ) const
{
	CK_OBJECT_HANDLE key = CK_INVALID_HANDLE;
	if( !d->findObject( CKO_PRIVATE_KEY, &key ) || key == CK_INVALID_HANDLE )
		return false;

	CK_MECHANISM mech = { CKM_RSA_PKCS, 0, 0 };
	if( d->f->C_VerifyInit( d->session, &mech, key ) != CKR_OK )
		return false;

	return d->f->C_Verify( d->session, (unsigned char*)data.constData(), data.size(), (unsigned char*)signature.data(), signature.size() ) == CKR_OK;
}
