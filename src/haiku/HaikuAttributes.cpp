/*
 * PowerMac emulator for Haiku - file attributes of shared folders.
 * Copyright 2026, the BeBox and PowerMac emulator authors. MIT License.
 */
#include "HaikuAttributes.h"

#include <Node.h>
#include <fs_attr.h>


bool
HaikuAttributes::Read(const std::string& path,
	std::vector<powermac::BfsAttribute>& attributes)
{
	BNode node(path.c_str());
	if (node.InitCheck() != B_OK)
		return false;
	char name[B_ATTR_NAME_LENGTH];
	while (node.GetNextAttrName(name) == B_OK) {
		attr_info info;
		if (node.GetAttrInfo(name, &info) != B_OK || info.size > (16 << 20))
			continue;
		powermac::BfsAttribute attribute;
		attribute.name = name;
		attribute.type = info.type;
		attribute.data.resize((size_t)info.size);
		if (node.ReadAttr(name, info.type, 0, attribute.data.data(),
				attribute.data.size()) != (ssize_t)attribute.data.size())
			continue;
		attributes.push_back(attribute);
	}
	return true;
}


bool
HaikuAttributes::Write(const std::string& path,
	const std::vector<powermac::BfsAttribute>& attributes)
{
	BNode node(path.c_str());
	if (node.InitCheck() != B_OK)
		return false;
	for (const powermac::BfsAttribute& attribute : attributes) {
		// replace: a shorter value must not keep the old tail
		node.RemoveAttr(attribute.name.c_str());
		node.WriteAttr(attribute.name.c_str(), attribute.type, 0,
			attribute.data.data(), attribute.data.size());
	}
	return true;
}


bool
HaikuAttributes::ReadAttribute(const std::string& path,
	const std::string& name, std::vector<powermac::u8>& data)
{
	BNode node(path.c_str());
	attr_info info;
	if (node.InitCheck() != B_OK
		|| node.GetAttrInfo(name.c_str(), &info) != B_OK
		|| info.size > (256 << 20))
		return false;
	data.resize((size_t)info.size);
	return node.ReadAttr(name.c_str(), info.type, 0, data.data(),
		data.size()) == (ssize_t)data.size();
}


bool
HaikuAttributes::AttributeSize(const std::string& path,
	const std::string& name, powermac::u64& size)
{
	BNode node(path.c_str());
	attr_info info;
	if (node.InitCheck() != B_OK
		|| node.GetAttrInfo(name.c_str(), &info) != B_OK)
		return false;
	size = (powermac::u64)info.size;
	return true;
}
